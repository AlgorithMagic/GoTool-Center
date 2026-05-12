#pragma once

#include "gotool_database.hpp"

namespace gotool::database {

static constexpr int64_t GOTOOL_SCHEMA_VERSION = 3;

void create_schema(Database &database, int64_t legacy_project_id = 0);

} // namespace gotool::database
