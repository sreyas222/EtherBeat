/*
 * Created by prabushitha on 6/12/18.
 * Copyright [2018] <ScoreLab Organization>
*/

#include "block_store.h"
#include "sql_statements.h"
#include "timing.h"
#include <sqlite3.h>
#include <iostream>
#include <sstream>
#include <cstdio>

#define BLOCKS_PER_SQLITE_TRANSACTION 10000

struct TransactionBuffer txbuffer;
void parseBlocks(sqlite3 *db_sqlite,
                 rocksdb::DB* db_rocks,
                 struct BuilderInfo &info,
                 EtherExtractor &extractor,
                 size_t start,
                 size_t end) {
    // start a transaction
    startTransaction(db_sqlite);


    sqlite3_stmt * stmt_block = nullptr;
    sqlite3_prepare(db_sqlite, sql_block, -1, &stmt_block, nullptr);

    sqlite3_stmt * stmt_tx = nullptr;
    sqlite3_prepare(db_sqlite, sql_tx, -1, &stmt_tx, nullptr);

    sqlite3_stmt * stmt_blocktx = nullptr;
    sqlite3_prepare(db_sqlite, sql_blocktx, -1, &stmt_blocktx, nullptr);

    sqlite3_stmt * stmt_txreceipt = nullptr;
    sqlite3_prepare(db_sqlite, sql_txreceipt, -1, &stmt_txreceipt, nullptr);

    sqlite3_stmt * stmt_fromto = nullptr;
    sqlite3_prepare(db_sqlite, sql_fromto, -1, &stmt_fromto, nullptr);

    bool isMasterOver = false;
    struct StoreBasicArgs basicArgs = {stmt_block,
    stmt_tx,
    stmt_blocktx,
    stmt_txreceipt,
    stmt_fromto,
    db_rocks,
    info,
    extractor,
    &isMasterOver,
    &txbuffer};

    // create a worker thread
    pthread_t tid;
    pthread_create(&tid, nullptr, consumer_store_transactions, reinterpret_cast<void *>(&basicArgs));
    // parse
    for (size_t i = start; i < end; i++) {
        Block block = extractor.getBlock(i);

        storeBlockInRDBMS(&basicArgs, block);
    }
    isMasterOver = true;
    pthread_cond_signal(&(basicArgs.txbuffer->wait_on_no_tx));
    pthread_join(tid, nullptr);

    // finalize
    sqlite3_finalize(stmt_block);
    sqlite3_finalize(stmt_tx);
    sqlite3_finalize(stmt_blocktx);
    sqlite3_finalize(stmt_txreceipt);
    sqlite3_finalize(stmt_fromto);

    endTransaction(db_sqlite);
}

int main(int argc, char* argv[]) {
    /*
    * Process user inputs
    */

    // setting default path
    std::string chain_path = "/mnt/rinkeby/geth/chaindata";
    std::string sqlite_path = "/tmp/dbsqlite";
    std::string rocks_path = "/tmp/dbrocks";
    if (argc < 2) {
        std::cout << "Mandatory to specify number of blocks to store in the 1st argument" << std::endl;
        std::cout << "Command format : EtherBuilder number_of_blocks [chain_path sqlite_path rocks_path]" << std::endl;
        std::cout << "Example :" << std::endl;
        std::cout << "./EtherBuilder 10000 /home/user/.ethereum/geth/chaindata /tmp/dbsqlite /tmp/dbrocks" << std::endl;
        return -1;
    }

    if (argc > 2) {
        std::istringstream cpath(argv[2]);
        cpath >> chain_path;
    }
    std::cout << "Using chain data path : " << chain_path << std::endl;

    std::istringstream iss(argv[1]);
    size_t total_blocks_to_store;
    iss >> total_blocks_to_store;
    std::cout << "Blocks to store = " << total_blocks_to_store << std::endl;

    if (argc > 3) {
        std::istringstream spath(argv[3]);
        spath >> sqlite_path;
    }
    if (argc > 4) {
        std::istringstream rpath(argv[4]);
        rpath >> rocks_path;
    }
    sqlite_path = sqlite_path + "/ethereum_blockchain.db";
    /*
    * Store data
    */
    // Connect to SQLite
    sqlite3 *db_sqlite;
    int rc = sqlite3_open(&sqlite_path[0], &db_sqlite);

    if (rc) {
        fprintf(stderr, "SQLite db failed : %s\n", sqlite3_errmsg(db_sqlite));
        return(0);
    } else {
        fprintf(stderr, "SQLite db connected\n");
    }

    // Connection to Rocksdb
    rocksdb::DB* db_rocks;
    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::Status status = rocksdb::DB::Open(options, rocks_path, &db_rocks);
    if (status.ok()) {
        printf("Rocksdb connected\n");
    } else {
        printf("Rocksdb failed : %s \n",  status.ToString().c_str());
    }

    struct BuilderInfo info;


    // get last info
    std::string nextBlockId;
    rocksdb::Status s1 = db_rocks->Get(rocksdb::ReadOptions(), info.KEY_NEXT_BLOCKID, &nextBlockId);
    std::string nextTxId;
    rocksdb::Status s2 = db_rocks->Get(rocksdb::ReadOptions(), info.KEY_NEXT_TXID, &nextTxId);
    std::string nextAddressId;
    rocksdb::Status s3 = db_rocks->Get(rocksdb::ReadOptions(), info.KEY_NEXT_ADDRESSID, &nextAddressId);

    if (s1.ok() && s2.ok() && s3.ok()) {
        printf("Rocks last %s\n Rocks last tx %s\n", nextBlockId.c_str(), nextTxId.c_str());
        info.nextBlockId = (size_t)std::stoi(nextBlockId);
        info.nextTxId = (size_t)std::stoi(nextTxId);
        info.nextAddressId = (size_t)std::stoi(nextAddressId);

    } else {
        printf("keys not found \n");
        info.nextBlockId = 0;
        info.nextTxId = 1;
        info.nextAddressId = 1;
    }

    // if the value are set to initial values, we need to start from the beginning.
    if (info.nextBlockId == 0 && info.nextTxId == 1) {
        // create database tables
        createRDBMSSchema(db_sqlite);
    }



    EtherExtractor extractor(chain_path);

    // Initializing thread parameters
    pthread_mutex_init(&(txbuffer.mutex), NULL);
    pthread_cond_init(&(txbuffer.wait_on_no_tx), NULL);
    pthread_cond_init(&(txbuffer.wait_on_full_tx), NULL);

    // Parsing
    std::cout << "starting block id \t" << info.nextBlockId << std::endl;
    std::cout << "starting tx id    \t" << info.nextTxId << std::endl;
    std::cout << "starting addr id  \t" << info.nextAddressId << std::endl;
    std::cout << "-------------------------------------" << std::endl;
    std::cout << "-----------PARSING STARTED-----------" << std::endl;
    std::cout << "-------------------------------------" << std::endl;

    // Start timing
    TimeCalculator timer{};
    timer.setStart();

    // Build started
    size_t parse_start_block = info.nextBlockId;
    size_t parse_end_block = total_blocks_to_store+1;  // info.nextBlockId+100000;

    if (parse_end_block < parse_start_block) {
        std::cout << "Blocks to "<< total_blocks_to_store <<" already stored" << std::endl;

        sqlite3_close(db_sqlite);
        delete db_rocks;
        return -1;
    }

    size_t total_blocks_to_parse = parse_end_block-parse_start_block;
    size_t total_sqlite_transactions = (total_blocks_to_parse/BLOCKS_PER_SQLITE_TRANSACTION)+1;
    if (total_blocks_to_parse == (total_sqlite_transactions-1)*BLOCKS_PER_SQLITE_TRANSACTION) total_sqlite_transactions -= 1;
    // if(total_sqlite_transactions == 0)  total_sqlite_transactions = 1;

    for (size_t i = 0; i < total_sqlite_transactions; i++) {
        size_t local_start = parse_start_block+BLOCKS_PER_SQLITE_TRANSACTION*i;
        size_t local_end = local_start+BLOCKS_PER_SQLITE_TRANSACTION;
        if (i == total_sqlite_transactions-1) {
            local_end = parse_end_block;
        }
        std::cout << "Blocks : " << local_start << " to "<< local_end << " (excluding)" << std::endl;

        parseBlocks(db_sqlite, db_rocks, info, extractor, local_start, local_end);
    }

    // Build over
    timer.printElapsedTime();
    std::cout << "----------PARSING COMPLETED----------" << std::endl;
    std::cout << "-------------------------------------" << std::endl;
    std::cout << "next block id \t" << info.nextBlockId << std::endl;
    std::cout << "next tx id    \t" << info.nextTxId << std::endl;
    std::cout << "next addr id  \t" << info.nextAddressId << std::endl;

    // save last ids
    s1 = db_rocks->Put(rocksdb::WriteOptions(), info.KEY_NEXT_BLOCKID, std::to_string(info.nextBlockId));
    s2 = db_rocks->Put(rocksdb::WriteOptions(), info.KEY_NEXT_TXID, std::to_string(info.nextTxId));
    s2 = db_rocks->Put(rocksdb::WriteOptions(), info.KEY_NEXT_ADDRESSID, std::to_string(info.nextAddressId));
    // close databases
    sqlite3_close(db_sqlite);
    delete db_rocks;

    return 0;
}
