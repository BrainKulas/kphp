---
sort: 5
---

# KPHP rewrite rules

KPHP uses its own DSL to express optimizations and various syntax rewrites.

This approach can be found in many compilers ([Go](https://github.com/golang/go/blob/master/src/cmd/compile/internal/ssa/_gen/generic.rules), [GCC](https://gcc.gnu.org/wiki/MELT%20tutorial), etc.).

This page describes the KPHP syntax of these rules, how and when to use them.


## High-level overview

The compiler needs to replace one AST tree with another from time to time.

Doing so in C++ can lead to the tedious code that requires comments in order
to explain which transformation we're doing. Describing this transformation in
declarative form can lead to more readable code. If the rules->C++ code generator
is good enough, we can also benefit from the increased performance and
less memory usage (but it's not the main goal).

Imagine the case when you want to replace a `microtime()` call with `_microtime_string()`
and `microtime(true)` with `_microtime_float()`.

Using the direct approach, you may end up with something like this:

```c++
if (call->func_id->name == "microtime") {
  // microtime() => _microtime_string()
  if (call->args().empty()) {
    call->set_string("_microtime_string");
    call->func_id = G->get_function(call->get_string());
    return call;
  }
  // microtime(true) => _microtime_float()
  if (call->args().size() == 1) {
    auto arg = VertexUtil::unwrap_bool_value(call->args()[0]);
    if (arg && arg->type() == op_true) {
      call->set_string("_microtime_float");
      call->func_id = G->get_function(call->get_string());
      return call;
    }
  }
}
```

Note that without comments that code requires some efforts to understand.
You'll also need to think how to properly add `microtime(false)` handling so it doesn't get ugly.

With rewrite rules, you'll need exactly 2 lines of "code":

```lisp
(op_func_call {"microtime"})           => (op_func_call {"_microtime_string"})
(op_func_call {"microtime"} (op_true)) => (op_func_call {"_microtime_float"})
```

It's obvious now how to add `microtime(false)` pattern.

The cognitive complexity is lower as well: you need to think about less weird details of the
compiler like using `unwrap_bool_value` to unpack the function argument.


## When to use

Although it can be tempting to express everything possible with rewrite rules, they have their limitations.

They work the best with a strict pattern structure that doesn't depend on the number of arguments.
When you find yourself writing 10+ patterns just because the pattern language is not expressive enough
you're probably better off with writing this vertex transformation by hand.

If the code generated by the rules generator (lisp->C++ translator) looks inefficient, the generator should be improved.
It should be able to generate the code that is good enough for the KPHP compiler engineer.

Here are some good examples for the rewrite rules:

* replacing one function call with another (potentially matching their arguments against another pattern)
* constant-fold some expressions (like `ord(const string)` => `resulting int literal`)

A bad example would be an optimization where you need to handle many combinations of related cases identically.
Patterns express `A => B` rules, so you would have to write `B` multiple times in that case.
Even if there was a syntax of executing the same rewrite action for multiple match patterns,
rules work better for the simpler cases.


## DSL manual

The rewrite rules file consists of a sequence of **rules** and optional comments.

Comments start with `#`.

```
rule         := matcher_expr [where_clause] [let_clause] => rewrite_expr
where_clause := "where" c_expr
let_clause   := ["if"] "let" VAR_NAME c_expr

matcher_expr := expr
rewrite_expr := expr

expr := simple_expr | [IDENT ":"] "(" OP [str_val_expr] expr... ")"

simple_expr  := IDENT | IDENT "..." | "..." 
str_val_expr := c_expr
c_expr       := "{" VERBATIM_STRING "}"

comment := ";;" .* "\n"
```

* `OP` is one of the vertex op constants (`op_func_call`, etc)
* `VERBATIM_STRING` is an expression that will be pasted into the generated code as is
* `args...` interpreted differently than `args ...` (read below)

> The `;;` is used for the comments to make most lisp syntax highlighting work out of the box

The simplest rule looks like this:

```lisp
;; concatenation of an empty string and some arbitrary expression "x";
;; replaced by a string conversion expr over that x
(op_concat (op_string {""}) x) => (op_conv_string x)
```

Some rules may require extra conditions to match described in `where`:

```lisp
;; ord() calls with a string literal argument that has a length of 1
;; the expression inside {} is evaluated at the compile time, by the compiler;
(op_func_call {"ord"} arg:(op_string))
  where { arg->str_val.size() == 1 }
  => (op_int_const { std::to_string(static_cast<unsigned char>(arg->str_val[0])) })
```

Note that `where` is only executed when the syntax does match.

Expressions in `matcher_expr` check whether some AST matches the pattern while `rewrite_expr` shows
how to construct a replacement AST for the successful match.

Some complex patterns may benefit from the local variables. Use `let` to introduce them.

```lisp
(op_func_call {"ord"} arg:(op_string))
  where { arg->str_val.size() == 1 }
  let int_value { std::to_string(static_cast<unsigned char>(arg->str_val[0])) }
  => (op_int_const {int_value})
```

When `let` is prefixed by `if`, an expression bound to the variable needs to evaluate to `true` in bool context,
otherwise the matching will be rejected. This is useful when you want to combine `where` and `let` into a single line.

If two variables inside the `matcher_expr` have the same name, they'll force the vertex equality check.
For instance, `(op_func_call {"f"} x x)` will match `f(1, 1)`, but not `f(1, 2)`.

```lisp
;; in other words, this pattern:
(op_func_call {"f"} x x) => ...

;; is functionally identical to this:
(op_func_call {"f"} x1 x2)
  where { is_same(x1, x2) }
  => ...
```

It's possible to use `_` variable name to accept any expression in that position. `(op_func_call {"f"} _ _)`
matches any `f` calls with exactly 2 arguments.

When you don't care about the rest of the arguments, you can use a trailing `...` argument. `(op_func_call {"f"} x ...)`
matches `f(1)`, `f($a, $b)`, but not `f()` (since it requires `x` to be present).

When you don't care about the arguments, but needs them in rewrite action, use a different `...` notation.
`(op_func_call {"f"} args...)` matches **any** function call, but `args` will be bound to a `VertexRange` which you can
use inside rewrite action.

```lisp
;; capturing xs and using it inside the rewrite action;
;; note that "c" is casted to VertexAdapter<op_func_call>, so we can access func_id inside "where"
(op_conv_int c:(op_func_call xs...))
  where { my_predicate(c->func_id) }
  => (op_func_call { c->name + "_foo" } xs)
```

To access the top-most vertex that is being matched, use `v_` variable:

```lisp
;; the "v_" variable always has a type of the top-most vertex pattern,
;; VertexAdaptor<op_func_call> in this case
(op_func_call {"f"} ...) where { !v_->func_id->is_extern() } => ...
```

## The pipeline integration

The rules_generator takes a rules source file and produces a C++ header and implementation files.

These C++ files are used when building the KPHP compiler.

The generated file implements a compiler pass that can be embedded into another pass.

Due to the complexity of our pipeline, you should be careful about where you're inserting the generated pass.
If unsure, use one of the existing pass that includes a generated rules pass.

The header file has one important function: `void run_{name}_rules_pass(FunctionPtr f)` where `name`
is the name of the rules file.

The function implementation may look like this:

```c++
void run_{name}_rules_pass(FunctionPtr f) {
  GeneratedPass pass{rewrite_rules::get_context()};
  run_function_pass(f, &pass);
}
```

The `GeneratedPass` is a class generated for the rules file. It's private to the `.cpp` file and can't be referenced from the outside.

When integrating this pass into another pass, call this function:

```c++
void MyPassF::execute(FunctionPtr f, DataStream<FunctionPtr> &os) {
  if (!f->is_extern()) {
    // you'll need to #include the generated ".h" file in order to call this function
    run_{name}_rules_pass(f);
  }
  os << f;
}
```

After updating or adding the patterns to the rules file, just recompile the KPHP compiler, and they'll be automatically re-generated.

The generated files are not committed to the C++ repository, just like the generated vertex definition files.


## Rules DSL best practices

When you need to perform a complex check inside `where` clause, consider adding a function to `rules_runtime.h` which
is supposed to be a support library included **only** from the generated C++ code.

```lisp
;; BAD: very complex condition inside a rules file
(op_func_call {"f"} ...)
  where { v_->func_id->return_typehint &&
          v_->func_id->return_typehint->try_as<TypeHintPrimitive>() &&
          v_->func_id->return_typehint->try_as<TypeHintPrimitive>()->ptype == tp_int }
  => ...
  
;; GOOD: using a helper function declared in either rules_runtime.h or somewhere else
(op_func_call {"f"} ...)
  where { typehint_ptype(v_->func_id->return_typehint) == tp_int }
  => ...
```

Use `;;` for line comments and `;;;` for header-like comments that separate the sections:

```lisp
;;; This is a header

;; this is an in-line comment
```

If `where` or `let` clause is present, put all clauses to a separate line after the match pattern:

```lisp
;; BAD: where and let are on the same line with everything
(op_func_call {"f"} ...) where { ... } let x { ... } => ...

;; GOOD: all clauses are on their own lines
(op_func_call {"f"} ...)
  where { ... }
  let x { ... }
  => ...
```

Write simple rules without `where`/`let` clauses as a single-line rule:

```lisp
;; BAD: a multi-line simple rule
(op_func_call {"f"} ...)
  => ...

;; GOOD: a single-line simple rule
(op_func_call {"f"} ...) => ...

;; Note: it's OK to split a rule into multiple lines if match or replacement actions are long
```

Try to avoid redundant repetition, use `if let` when it makes the rules more concise:

```lisp
;; BAD: repetitive where+let
(op_index arr k)
  where { can_convert_to_tmp_string_expr(k) }
  let k2 { to_tmp_string_expr(k) }
  => (op_index arr k2)

;; BAD: even more repetitive
(op_index arr k)
  where { to_tmp_string_expr(k) }
  let k2 { to_tmp_string_expr(k) }
  => (op_index arr k2)

;; GOOD: using if-let
(op_index arr k)
  if let k2 { to_tmp_string_expr(k) }
  => (op_index arr k2)
```

Do not add spaces near `{}` for simple expressions like literals, add spaces for C++ snippets ("complex expressions"):

```lisp
;; BAD: redundant spaces
(op_func_call { "f" } ...) => ...
;; BAD: no spaces around the complex expression
(op_func_call ...) => (op_func_call {"prefix_" + v_->func_id->name} ...)

;; GOOD: no redundant spaces
(op_func_call {"f"} ...) => ...
;; GOOD: spaces around the complex expression
(op_func_call ...) => (op_func_call { "prefix_" + v_->func_id->name } ...)
```

When describing a replacement expression, try to re-use the existing vertices from the match pattern:

```lisp
;; BAD: allocating a new conv_array vertex
(op_func_call {"f"} (op_conv_array x)) => (op_func_call {"f2"} (op_conv_array x))

;; GOOD: reusing the conv_array vertex
(op_func_call {"f"} conv:(op_conv_array x)) => (op_func_call {"f2"} conv)
```

Use short pattern variable names. Rules have a very limited scope (1-3 lines) and context, there is no need to make longer names:

```lisp
;; BAD: names are longer than they could be
(op_func_call {"f"} array_conversion:(op_conv_array conv_argument)) => (op_func_call {"f2"} array_conversion)

;; GOOD: names are short
(op_func_call {"f"} conv:(op_conv_array x)) => (op_func_call {"f2"} conv)

;; note that you can use "_" as a name instead of "x" here, since we don't really
;; care about conv contents (see next recommendation)
```

When pattern variable is not used in `where`/`let` or replacement pattern, consider using `_` identifier name (unless variable names improve the readability):

```lisp
;; GOOD: using "_" as a variable name without reduced readability
(op_func_call {"f"} conv:(op_conv_array _)) => (op_func_call {"f2"} conv)
```

## Debugging the rules

When debugging the rules, you can put a breakpoint to the auto-generated code that represents a matching of the rule you're going to debug.

Let's assume that you want to debug a `(op_string_build x) => (op_conv_string x)` rule that is defined inside `early_opt.rules` file.

The generated code could look like this:

```c++
  VertexPtr on_op_string_build_(VertexAdaptor<op_string_build> v_) {
    using namespace rewrite_rules;
    
    // early_opt.rules:59
    //   pattern: (op_string_build x)
    //   rewrite: (op_conv_string x)
    do {
      if (v_->args().size() != 1) { break; }
      const auto &x = v_->args()[0];
      auto replacement_ = create_vertex<op_conv_string>(ctx_, x);
      replacement_.set_location_recursively(v_);
      return replacement_;
    } while (false);
    
    return v_; // no matches
  }
```

You can easily find the source location (`early_opt.rules:59`) and insert a breakpoint anywhere you like.

If you want to add some debug prints, use this trick:

```lisp
;; the rule_debug() prints its arguments and returns true
(op_string_build x)
  where { rule_debug("some message") }
  => (op_conv_string x)
```

With this, every time your rule matches it will print "some message".


## Code generation details

It's important to understand how rules->C++ translation works if you want to extend the rules DSL, make the generated code better
or fix some bug related to it.

The syntax is simple and resembles S-expressions (a notation most often used in lisps).
That syntax is handled by `rules_lexer.py` and `rules_parser.py`.
They produce a parse tree that is traversed by a `rules_generator.py` to produce a C++ code that
implements the rules defined in the `.rules` file.

The code generator knows everything it needs about the vertex structures; it uses the
schema file (`vertex-desc.json`) to get the relevant details.

If you need to add a DSL feature, you'll probably need to modify all the components (except the lexer maybe).
If you need to change the C++ output, you'll only need to modify the `rules_generator.py`.

The `Context` class is used as a per-thread cache that helps the generated code to allocate
fewer vertices during its work. The vertices to be retired (thrown away) are added to the cache,
so the next time we want to create a vertex of the same type (and size) we can take it from that cache.
We're only caching the most often used vertex types, like function calls.

Since the cache (`Context`) is thread-local, it doesn't need any synchronization.

```c++
Context &get_context() {
  static thread_local Context ctx;
  return ctx;
}
```

Whether possible, the generated code will try to modify the vertex in-place to avoid allocating a new vertex.
