// Compiler for PHP (aka KPHP)
// Copyright (c) 2020 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#include "compiler/code-gen/files/tl2cpp/tl2cpp.h"

#include "common/tlo-parsing/tlo-parsing.h"

#include "compiler/code-gen/files/tl2cpp/tl-module.h"
#include "compiler/code-gen/files/tl2cpp/tl2cpp-utils.h"
#include "compiler/code-gen/naming.h"
#include "compiler/code-gen/raw-data.h"

/* There are 3 important kinds of types (each with its own store and fetch methods):
 * 1) Function    - an entry point for every TL query. We start de\serialization of every query from here.
 *                  It has fields/arguments (such structure names start with "f_").
 * 2) Type        - a property of almost every combinator (Function or Constructor).
 *                  de\serialization of any field starts with a store/fetch call of appropriate Type (struct t_...).
 *                  Implemented as a switch over 'magic' followed by a matching Constructor call.
 * 3) Constructor - one of the variants for the concrete type data (a component type of the sum type).
 *                  store/fetch of the Constructor should ONLY be called from the associated Type store/fetch functions.
 *                  Generated structures names start with "c_".
 *
 * @@@ Structures for the code generation @@@
 * Names start with Function/Type/Constructor/Combinator/TypeExpr.
 * Structures that start the code generation with their compile() call:
 * 1) <*>Decl - structure declaration generation.
 * 2) <*>Def  - structure definition generation.
 * where <*> - Function, Type or Constructor
 *
 * Then we always call a compile() call for these structures:
 * 1) <*>Store - store() function generation.
 * 2) <*>Fetch - fetch() function generation.
 * where <*> - Combinator or Type
 * Combinator - is a function or a constructor.
 * They have identical semantics, therefore store/fetch is generated by the same code.
 *
 * After that we always call a compile() for these:
 * 1) TypeExprStore
 * 2) TypeExprFetch
 * Code generation of the fetching (or storing) of the concrete type expression,
 * for example Maybe (%tree_stats.PeriodsResult fields_mask)
 */
namespace tl2cpp {

/* Bundle all combinators and types by modules while collecting all their dependencies along the way.
 * */
static void collect_target_objects() {
  auto should_exclude_tl_type = [](const std::unique_ptr<vk::tlo_parsing::type> &t) {
    return CUSTOM_IMPL_TYPES.find(t->name) != CUSTOM_IMPL_TYPES.end()
           || t->name == "engine.Query";                        // it's the only type with !X; can we get rid of it in the future?
  };
  auto should_exclude_tl_function = [](const std::unique_ptr<vk::tlo_parsing::combinator> &f) {
    return !G->settings().gen_tl_internals.get() && f->is_internal_function();
  };

  for (const auto &e : tl->types) {
    const std::unique_ptr<vk::tlo_parsing::type> &t = e.second;
    if (!should_exclude_tl_type(t)) {
      Module::add_to_module(t);
    }
  }

  for (const auto &e : tl->functions) {
    const std::unique_ptr<vk::tlo_parsing::combinator> &f = e.second;
    if (!should_exclude_tl_function(f)) {
      Module::add_to_module(f);
    }
  }
}

void write_rpc_server_functions(CodeGenerator &W) {
  W << OpenFile("rpc_server_fetch_request.cpp", "tl", false);
  W << ExternInclude(G->settings().runtime_headers.get());
  std::vector<vk::tlo_parsing::combinator *> kphp_functions;
  IncludesCollector deps;
  deps.add_raw_filename_include("tl/common.h"); // to make it successfully compile even if there are no @kphp functions
  for (const auto &item : tl->functions) {
    const auto &f = item.second;
    if (f->is_kphp_rpc_server_function()) {
      auto klass = get_php_class_of_tl_function(f.get());
      if (klass) { // With !klass we assume that such function doesn't exist
        kphp_functions.emplace_back(f.get());
        deps.add_class_include(klass);
        deps.add_raw_filename_include("tl/" + Module::get_module_name(f->name) + ".h");
      }
    }
  }
  W << deps << NL;
  W << ExternInclude{G->settings().runtime_headers.get()} << NL;
  FunctionSignatureGenerator(W) << "class_instance<C$VK$TL$RpcFunction> f$rpc_server_fetch_request() " << BEGIN;
  W << "auto function_magic = static_cast<unsigned int>(rpc_fetch_int());" << NL;
  W << "switch(function_magic) " << BEGIN;
  for (const auto &f : kphp_functions) {
    W << fmt_format("case {:#010x}: ", static_cast<unsigned int>(f->id)) << BEGIN;
    W << get_php_runtime_type(f, true) << " request;" << NL
      << "request.alloc();" << NL
      << "CurrentRpcServerQuery::get().save(" << cpp_tl_struct_name("f_", f->name) << "::rpc_server_typed_fetch(request.get()));" << NL
      << "return request;" << NL
      << END << NL;
  }
  W << "default: " << BEGIN
    << "php_warning(\"Unexpected function magic on fetching request in rpc server: 0x%08x\", function_magic);" << NL
    << "return {};" << NL
    << END << NL;
  W << END << NL;
  W << END << NL;
  W << CloseFile();
}

void write_tl_query_handlers(CodeGenerator &W) {
  if (G->settings().tl_schema_file.get().empty()) {
    return;
  }

  auto parsing_result = vk::tlo_parsing::parse_tlo(G->settings().tl_schema_file.get().c_str(), false);
  kphp_error_return(parsing_result.parsed_schema,
                    fmt_format("Error while reading tlo: {}", parsing_result.error));

  tl = parsing_result.parsed_schema.get();
  try {
    vk::tlo_parsing::replace_anonymous_args(*tl);
    vk::tlo_parsing::perform_flat_optimization(*tl);
    vk::tlo_parsing::tl_scheme_final_check(*tl);
  } catch (const std::exception &ex) {
    kphp_error_return(false, ex.what());
  }
  collect_target_objects();
  for (const auto &e : modules) {
    const Module &module = e.second;
    W << module;
  }

  if (G->get_untyped_rpc_tl_used()) {
    W << OpenFile("tl_runtime_bindings.cpp", "tl", false);
    W << ExternInclude(G->settings().runtime_headers.get());
    for (const auto &module_name : modules_with_functions) {
      W << Include("tl/" + module_name + ".h");
    }
    W << NL;
    // a pointer to the gen$tl_fetch_wrapper is forwarded to the runtime and called from the fetch_function();
    // we do this to avoid t_ReqResult-related dependencies inside the runtime
    FunctionSignatureGenerator(W) << "array<mixed> gen$tl_fetch_wrapper(std::unique_ptr<tl_func_base> stored_fetcher) " << BEGIN
                                  << "tl_exclamation_fetch_wrapper X(std::move(stored_fetcher));" << NL
                                  << "return t_ReqResult<tl_exclamation_fetch_wrapper, 0>(std::move(X)).fetch();" << NL
                                  << END << NL << NL;
    // a hash table that contains all TL functions;
    // it's passed to the runtime just like the fetch wrapper
    W << "array<tl_storer_ptr> gen$tl_storers_ht;" << NL;
    FunctionSignatureGenerator(W) << "void fill_tl_storers_ht() " << BEGIN;
    for (const auto &module_name : modules_with_functions) {
      for (const auto &f : modules[module_name].target_functions) {
        W << "gen$tl_storers_ht.set_value(" << register_tl_const_str(f->name) << ", " << "&" << cpp_tl_struct_name("f_", f->name) << "::store, "
          << hash_tl_const_str(f->name) << "L);" << NL;
      }
    }
    W << END << NL;
    W << CloseFile();
  }
  write_rpc_server_functions(W);
  // Const string literals that were extracted from the scheme
  W << OpenFile("tl_const_vars.h", "tl");
  W << "#pragma once" << NL;
  W << ExternInclude(G->settings().runtime_headers.get());
  std::for_each(tl_const_vars.begin(), tl_const_vars.end(), [&W](const std::string &s) {
    W << "extern string " << cpp_tl_const_str(s) << ";" << NL;
  });
  W << CloseFile();

  W << OpenFile("tl_const_vars.cpp", "tl", false);
  W << ExternInclude(G->settings().runtime_headers.get());
  W << Include("tl/tl_const_vars.h");
  std::for_each(tl_const_vars.begin(), tl_const_vars.end(), [&W](const std::string &s) {
    W << "string " << cpp_tl_const_str(s) << ";" << NL;
  });
  auto const_string_shifts = compile_raw_data(W, tl_const_vars);
  FunctionSignatureGenerator(W) << "void tl_str_const_init() " << BEGIN;
  size_t i = 0;
  std::for_each(tl_const_vars.begin(), tl_const_vars.end(), [&](const std::string &s) {
    W << cpp_tl_const_str(s) << ".assign_raw (&raw[" << const_string_shifts[i++] << "]);" << NL;
  });
  W << END << NL << NL;

  // to decode tl magics to strings at runtime, codegen everything
  // having two linear arrays (tl_magic_ids and tl_magic_names) is enough for our cases and compiles fast
  // see runtime/tl/tl_magics_decoding.cpp

  W << "static const unsigned int tl_magic_ids[" << tl->functions.size() << "] = " << BEGIN;
  for (const auto &[magic, tl_fun] : tl->functions) {
    W << fmt_format("{:#010x}U,", static_cast<unsigned int>(tl_fun->id)) << NL;
  }
  W << END << ";" << NL;

  W << "static const char *tl_magic_names[" << tl->functions.size() << "] = " << BEGIN;
  for (const auto &[magic, tl_fun] : tl->functions) {
    W << "\"" << tl_fun->name.c_str() << "\"," << NL;
  }
  W << END << ";" << NL << NL;

  W << "void tl_magic_fill_all_functions_impl(array<string> &out) noexcept " << BEGIN
    << "out.reserve(" << tl->functions.size() << ", false);" << NL
    << "for (int i=0; i<" << tl->functions.size() << "; ++i)" << BEGIN
    << "out.set_value(static_cast<int64_t>(tl_magic_ids[i]), string(tl_magic_names[i]));" << NL << END
    << NL << END << NL;

  W << "const char *tl_magic_convert_to_name_impl(unsigned int magic) noexcept " << BEGIN
    << "for (int i=0; i<" << tl->functions.size() << "; ++i)" << BEGIN
    << "if (tl_magic_ids[i] == magic) return tl_magic_names[i];" << NL << END
    << "return \"__unknown__\";"
    << NL << END << NL;

  W << CloseFile();

  modules.clear();
  modules_with_functions.clear();
}
} // namespace tl_gen

void TlSchemaToCpp::compile(CodeGenerator &W) const {
  tl2cpp::write_tl_query_handlers(W);
}