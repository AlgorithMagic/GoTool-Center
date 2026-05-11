// register_types.cpp
#include "register_types.h"

#include "project_context/gotool_project_context.hpp"
#include "project_scanner/gotool_project_scanner.hpp"

#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_gotool_types(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    ClassDB::register_class<ProjectScanner>();
    ClassDB::register_class<GodotProjectContext>();
}

void uninitialize_gotool_types(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
}

extern "C" {

GDExtensionBool GDE_EXPORT gotool_library_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address,
    const GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization *r_initialization
) {
    GDExtensionBinding::InitObject init_obj(
        p_get_proc_address,
        p_library,
        r_initialization
    );

    init_obj.register_initializer(initialize_gotool_types);
    init_obj.register_terminator(uninitialize_gotool_types);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

    return init_obj.init();
}

}
