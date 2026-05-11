#include "gotool_database.hpp"

#include <utility>

namespace gotool::database {

namespace {

void throw_sqlite_error(sqlite3 *db, const std::string &context, int code = SQLITE_ERROR) {
    const std::string message =
        db != nullptr
            ? sqlite3_errmsg(db)
            : "sqlite database handle was null";

    throw std::runtime_error(
        context + " (code " + std::to_string(code) + "): " + message
    );
}

void ensure_sqlite_ok(sqlite3 *db, int code, const std::string &context) {
    if (code != SQLITE_OK) {
        throw_sqlite_error(db, context, code);
    }
}

void ensure_sqlite_done(sqlite3 *db, int code, const std::string &context) {
    if (code != SQLITE_DONE) {
        throw_sqlite_error(db, context, code);
    }
}

} // namespace

Statement::Statement(sqlite3 *db, const std::string &sql) :
    db_(db) {
    if (db_ == nullptr) {
        throw std::runtime_error("Cannot prepare statement: sqlite database handle was null.");
    }

    const int code = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr);
    ensure_sqlite_ok(db_, code, "Failed to prepare sqlite statement");
}

Statement::~Statement() noexcept {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

Statement::Statement(Statement &&other) noexcept {
    db_ = other.db_;
    stmt_ = other.stmt_;
    other.db_ = nullptr;
    other.stmt_ = nullptr;
}

Statement &Statement::operator=(Statement &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
    }

    db_ = other.db_;
    stmt_ = other.stmt_;
    other.db_ = nullptr;
    other.stmt_ = nullptr;
    return *this;
}

void Statement::bind_int64(int index, int64_t value) {
    const int code = sqlite3_bind_int64(stmt_, index, value);
    ensure_sqlite_ok(db_, code, "Failed to bind sqlite int64 parameter");
}

void Statement::bind_text(int index, const std::string &value) {
    const int code = sqlite3_bind_text(
        stmt_,
        index,
        value.c_str(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT
    );
    ensure_sqlite_ok(db_, code, "Failed to bind sqlite text parameter");
}

void Statement::step_done() {
    const int code = sqlite3_step(stmt_);
    ensure_sqlite_done(db_, code, "Failed while finishing sqlite statement");
}

Database::Database(const std::string &database_path) {
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;

    const int result = sqlite3_open_v2(
        database_path.c_str(),
        &db_,
        flags,
        nullptr
    );

    if (result != SQLITE_OK) {
        const std::string message =
            db_ != nullptr
                ? sqlite3_errmsg(db_)
                : "sqlite3_open_v2 failed before creating a database handle";

        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }

        throw std::runtime_error("Failed to open GoTool database: " + message);
    }

    sqlite3_extended_result_codes(db_, 1);

    exec("PRAGMA foreign_keys = ON;");
    exec("PRAGMA journal_mode = WAL;");
    exec("PRAGMA busy_timeout = 5000;");
}

Database::~Database() noexcept {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Database::exec(const std::string &sql) {
    char *error_message = nullptr;

    const int result = sqlite3_exec(
        db_,
        sql.c_str(),
        nullptr,
        nullptr,
        &error_message
    );

    if (result != SQLITE_OK) {
        const std::string message =
            error_message != nullptr
                ? error_message
                : sqlite3_errmsg(db_);

        sqlite3_free(error_message);

        throw std::runtime_error("GoTool SQLite error: " + message);
    }
}

Statement Database::prepare(const std::string &sql) {
    return Statement(db_, sql);
}

void Database::begin_immediate_transaction() {
    exec("BEGIN IMMEDIATE TRANSACTION;");
}

void Database::commit_transaction() {
    exec("COMMIT;");
}

void Database::rollback_transaction() noexcept {
    if (db_ == nullptr) {
        return;
    }

    char *error_message = nullptr;
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &error_message);
    sqlite3_free(error_message);
}

int64_t Database::last_insert_row_id() const {
    return sqlite3_last_insert_rowid(db_);
}

Transaction::Transaction(Database &database) :
    database_(&database) {
    database_->begin_immediate_transaction();
}

Transaction::~Transaction() noexcept {
    if (!committed_ && database_ != nullptr) {
        database_->rollback_transaction();
    }
}

void Transaction::commit() {
    if (database_ == nullptr || committed_) {
        return;
    }

    database_->commit_transaction();
    committed_ = true;
}

} // namespace gotool::database
