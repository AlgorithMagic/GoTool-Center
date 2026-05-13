#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gotool::database {

class Statement {
public:
    enum class StepResult {
        Row,
        Done
    };

    Statement(sqlite3 *db, const std::string &sql);
    ~Statement() noexcept;

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    Statement(Statement &&other) noexcept;
    Statement &operator=(Statement &&other) noexcept;

    void bind_int64(int index, int64_t value);
    void bind_text(int index, const std::string &value);
    void bind_text(int index, const char *value);
    void bind_text(int index, std::string_view value);
    void bind_null(int index);
    StepResult step();
    void step_done();
    void reset();
    void clear_bindings();
    bool column_is_null(int index) const;
    int64_t column_int64(int index) const;
    std::string column_text(int index) const;

private:
    sqlite3 *db_ = nullptr;
    sqlite3_stmt *stmt_ = nullptr;
};

class Database {
public:
    explicit Database(const std::string& database_path);
    ~Database() noexcept;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    void exec(const std::string& sql);

    Statement prepare(const std::string &sql);

    void begin_immediate_transaction();
    void commit_transaction();
    void rollback_transaction() noexcept;

    int64_t last_insert_row_id() const;
    int64_t changes() const;

private:
    sqlite3* db_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(Database &database);
    ~Transaction() noexcept;

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    void commit();

private:
    Database *database_ = nullptr;
    bool committed_ = false;
};

} // namespace gotool::database
