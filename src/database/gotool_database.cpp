#include "gotool_database.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace gotool::database {

namespace {

[[noreturn]] void throw_sqlite_error(sqlite3 *db, const std::string &context, int code = SQLITE_ERROR) {
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

void Statement::bind_null(int index) {
    const int code = sqlite3_bind_null(stmt_, index);
    ensure_sqlite_ok(db_, code, "Failed to bind sqlite null parameter");
}

Statement::StepResult Statement::step() {
    const int code = sqlite3_step(stmt_);

    if (code == SQLITE_ROW) {
        return StepResult::Row;
    }

    if (code == SQLITE_DONE) {
        return StepResult::Done;
    }

    throw_sqlite_error(db_, "Failed while stepping sqlite statement", code);
}

void Statement::step_done() {
    if (step() != StepResult::Done) {
        throw std::runtime_error("Expected sqlite statement to finish with SQLITE_DONE but received a row.");
    }
}

void Statement::reset() {
    const int code = sqlite3_reset(stmt_);
    ensure_sqlite_ok(db_, code, "Failed to reset sqlite statement");
}

void Statement::clear_bindings() {
    const int code = sqlite3_clear_bindings(stmt_);
    ensure_sqlite_ok(db_, code, "Failed to clear sqlite statement bindings");
}

bool Statement::column_is_null(int index) const {
    return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

int64_t Statement::column_int64(int index) const {
    return sqlite3_column_int64(stmt_, index);
}

std::string Statement::column_text(int index) const {
    const unsigned char *text = sqlite3_column_text(stmt_, index);

    if (text == nullptr) {
        return "";
    }

    const int bytes = sqlite3_column_bytes(stmt_, index);

    if (bytes <= 0) {
        return "";
    }

    return std::string(reinterpret_cast<const char *>(text), static_cast<size_t>(bytes));
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
    exec("PRAGMA synchronous = NORMAL;");
    exec("PRAGMA temp_store = MEMORY;");
    exec("PRAGMA busy_timeout = 5000;");
}

Database::~Database() noexcept {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Database::exec(const std::string &sql) {
    if (db_ == nullptr) {
        throw std::runtime_error("Cannot execute sqlite statement: database handle was null.");
    }

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
    if (db_ == nullptr) {
        throw std::runtime_error("Cannot prepare sqlite statement: database handle was null.");
    }

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
    if (db_ == nullptr) {
        return 0;
    }

    return sqlite3_last_insert_rowid(db_);
}

int64_t Database::changes() const {
    if (db_ == nullptr) {
        return 0;
    }

    return sqlite3_changes64(db_);
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
