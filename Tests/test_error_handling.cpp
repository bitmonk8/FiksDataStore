#include "../Lib/lmdb.h"
#include <iostream>
#include <cassert>

int main()
{
    MDB_env* env;
    MDB_txn* txn;
    MDB_dbi dbi;
    int rc;

    std::cout << "Testing error handling for removed duplicate functionality...\n";

    // Initialize environment
    rc = mdb_env_create(&env);
    assert(rc == 0);

    rc = mdb_env_set_mapsize(env, 10485760); // 10MB
    assert(rc == 0);

    rc = mdb_env_open(env, "./testdb_error", MDB_FIXEDMAP | MDB_NOSYNC, 0664);
    assert(rc == 0);

    // Begin transaction
    rc = mdb_txn_begin(env, NULL, 0, &txn);
    assert(rc == 0);

    // Test 1: Try to open database with MDB_DUPSORT flag (should fail)
    std::cout << "Test 1: Opening database with MDB_DUPSORT flag...\n";
    rc = mdb_dbi_open(txn, "test_dup", MDB_DUPSORT | MDB_CREATE, &dbi);
    if (rc == MDB_INCOMPATIBLE) {
        std::cout << "✓ MDB_DUPSORT correctly rejected with MDB_INCOMPATIBLE\n";
    } else {
        std::cout << "✗ Expected MDB_INCOMPATIBLE, got: " << rc << "\n";
        return 1;
    }

    // Test 2: Try to open database with MDB_DUPFIXED flag (should fail)
    std::cout << "Test 2: Opening database with MDB_DUPFIXED flag...\n";
    rc = mdb_dbi_open(txn, "test_dupfixed", MDB_DUPFIXED | MDB_CREATE, &dbi);
    if (rc == MDB_INCOMPATIBLE) {
        std::cout << "✓ MDB_DUPFIXED correctly rejected with MDB_INCOMPATIBLE\n";
    } else {
        std::cout << "✗ Expected MDB_INCOMPATIBLE, got: " << rc << "\n";
        return 1;
    }

    // Test 3: Try to open database with both flags (should fail)
    std::cout << "Test 3: Opening database with both MDB_DUPSORT and MDB_DUPFIXED flags...\n";
    rc = mdb_dbi_open(txn, "test_both", MDB_DUPSORT | MDB_DUPFIXED | MDB_CREATE, &dbi);
    if (rc == MDB_INCOMPATIBLE) {
        std::cout << "✓ Combined duplicate flags correctly rejected with MDB_INCOMPATIBLE\n";
    } else {
        std::cout << "✗ Expected MDB_INCOMPATIBLE, got: " << rc << "\n";
        return 1;
    }

    // Test 4: Open a normal database (should succeed)
    std::cout << "Test 4: Opening normal database...\n";
    rc = mdb_dbi_open(txn, "test_normal", MDB_CREATE, &dbi);
    if (rc == 0) {
        std::cout << "✓ Normal database opened successfully\n";
        mdb_dbi_close(env, dbi);
    } else {
        std::cout << "✗ Failed to open normal database: " << rc << "\n";
        return 1;
    }

    mdb_txn_abort(txn);
    mdb_env_close(env);

    std::cout << "All error handling tests passed!\n";
    return 0;
}