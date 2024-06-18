add_library(cc INTERFACE)
target_include_directories(cc INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_sources(cc INTERFACE
    cc.c cc.h cc_extrns.h cc_tokns.h cc_ops.h cc_defs.h
    cc_malloc.c cc_malloc.h
    cc_wraps.c cc_wraps.h
    cc_ast.c cc_ast.h
    cc_parse.c cc_parse.h
    cc_gen.c cc_gen.h
    cc_peep.c cc_peep.h
    cc_printf.S
)
