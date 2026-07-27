; SPDX-License-Identifier: GPL-2.0-or-later

(define krudd-root (or (getenv "KRUDD_ROOT") "."))
(load (string-append krudd-root "/krudd/kruddmake/introspect.scm"))

(define fail-count 0)
(define (check name ok)
  (if ok
      (display (string-append "  ok    " name "\n"))
      (begin (set! fail-count (+ fail-count 1))
             (display (string-append "  FAIL  " name "\n")))))

(define (has? s sub)
  (let ((hl (string-length s)) (nl (string-length sub)))
    (let loop ((i 0))
      (cond ((> (+ i nl) hl) #f)
            ((string=? (substring s i (+ i nl)) sub) #t)
            (else (loop (+ i 1)))))))

(define (all-digits? s)
  (and (> (string-length s) 0)
       (let loop ((i 0))
         (cond ((>= i (string-length s)) #t)
               ((char-numeric? (string-ref s i)) (loop (+ i 1)))
               (else #f)))))

(define (slurp path)
  (call-with-input-file path
    (lambda (p)
      (let loop ((cs '()) (c (read-char p)))
        (if (eof-object? c) (list->string (reverse cs))
            (loop (cons c cs) (read-char p)))))))

(display "introspect: helpers\n")
(check "strip trims whitespace/newlines"
       (string=? (krudd-strip "  6.3.2\n\t") "6.3.2"))
(check "split on dot" (equal? (krudd-split "6.3.2" #\.) '("6" "3" "2")))
(check "replace all occurrences"
       (string=? (krudd-replace "a@X@b@X@c" "@X@" "1") "a1b1c"))

(check "version falls back to the dev placeholder outside CI"
       (string=? (krudd-version) "0.0.0-dev"))
(check "version-core strips a CI prerelease suffix"
       (string=? (krudd-version-core "10.1.0-pr482+a1b2c3d") "10.1.0"))
(check "version-core is a no-op on a bare version"
       (string=? (krudd-version-core "10.1.0") "10.1.0"))
(check "commit hash non-empty" (> (string-length (krudd-commit-hash)) 0))

(define version (krudd-version))

(display "introspect: codegen\n")
(define tmp (string-append krudd-root "/build/_introspect_test"))
(system (string-append "mkdir -p " tmp))

(krudd-configure-file
 (string-append krudd-root "/krudd/engine/core/version.h.in")
 (string-append tmp "/version.h"))
(let ((v (slurp (string-append tmp "/version.h"))))
  (check "version.h carries the literal version"
         (has? v (string-append "ENGINE_VERSION_STRING \"" version "\"")))
  (check "version.h has no unexpanded @VAR@ tokens" (not (has? v "@")))
  (check "version.h defines the patch component numerically"
         (all-digits? (list-ref (krudd-split (krudd-version-core version) #\.) 2)))
  (check "version.h has no leftover build-number macro"
         (not (has? v "ENGINE_BUILD_NUMBER")))
  (check "version.h has no leftover chore macro"
         (not (has? v "ENGINE_VERSION_CHORE"))))

(krudd-embed-file
 (string-append krudd-root "/krudd/engine/core/runtime.scm")
 (string-append tmp "/runtime_scm.h") "RUNTIME_SCM")
(let ((h (slurp (string-append tmp "/runtime_scm.h"))))
  (check "embed header declares the symbol"
         (has? h "static const char RUNTIME_SCM[] ="))
  (check "embed body is a NUL-terminated byte array"
         (and (has? h "(char)0x") (has? h "(char)0x00"))))

(display "introspect: binding generator\n")
(krudd-embed-scheme-module
 (string-append krudd-root "/krudd/engine/ui/kruddboard/md_parse.scm")
 (string-append tmp "/md_parse.h")
 (string-append tmp "/md_parse.scm.c"))

(let ((h (slurp (string-append tmp "/md_parse.h"))))
  (check "header has the include guard" (has? h "#ifndef MD_PARSE_H"))
  (check "header folds in the constants" (has? h "#define MD_TEXT_MAX 256"))
  (check "header is extern C safe" (has? h "extern \"C\""))
  (check "header emits struct md_span" (has? h "struct md_span {"))
  (check "header emits the char field bound"
         (has? h "char text[MD_TEXT_MAX];"))
  (check "header emits the vector field"
         (has? h "struct md_span spans[MD_SPANS_PER_BLOCK];"))
  (check "header emits the prototype"
         (has? h "int32_t md_parse(const char *src, struct md_block *out, int32_t max);"))
  (check "declaration forms do not leak into C"
         (not (has? h "define-c-struct"))))

(let ((c (slurp (string-append tmp "/md_parse.scm.c"))))
  (check "shim includes the generated header" (has? c "#include \"md_parse.h\""))
  (check "shim bakes the image in" (has? c "static const char md_parse_scm_image[]"))
  (check "shim generates each struct marshaler"
         (and (has? c "md_parse_marshal_md_span")
              (has? c "md_parse_marshal_md_block")))
  (check "shim asserts marshal arity" (has? c "s7_list_length(s7, v) == 4"))
  (check "shim clamps the char field" (has? c "n > MD_TEXT_MAX - 1"))
  (check "shim drives the scheme proc"
         (has? c "s7_name_to_value(s7, \"md-parse\")"))
  (check "shim emits the driver"
         (has? c "int32_t md_parse(const char *src, struct md_block *out, int32_t max)")))

(display "introspect: interface header\n")
(krudd-emit-interface-header
 (string-append krudd-root "/krudd/engine/render/renderer.scm")
 (string-append tmp "/renderer.h"))

(let ((h (slurp (string-append tmp "/renderer.h"))))
  (check "header has the include guard" (has? h "#ifndef RENDERER_H"))
  (check "header pulls in its c-include forms"
         (and (has? h "#include <stddef.h>") (has? h "#include <stdint.h>")))
  (check "handle becomes an opaque typedef"
         (has? h "typedef struct gpu_cmd_buf *gpu_cmd_buf_t;"))
  (check "enum folds to a typedef enum with screaming members"
         (and (has? h "typedef enum {")
              (has? h "GPU_CAP_DRAW_DIRECT = 1u << 0,")
              (has? h "} gpu_cap;")))
  (check "auto-valued enum member carries no initializer"
         (has? h "GPU_FORMAT_RGBA8_UNORM,"))
  (check "define renders bare int and parenthesized shift"
         (and (has? h "#define GPU_MAX_VERTEX_ATTRS 8")
              (has? h "#define GPU_STAGE_TOP (1u << 0)")))
  (check "scalar typedef alias emitted"
         (has? h "typedef uint32_t gpu_stage_mask;"))
  (check "struct field with a #define-sized array of structs"
         (has? h "struct gpu_vertex_attr attrs[GPU_MAX_VERTEX_ATTRS];"))
  (check "pointer-to-const field hugs the name"
         (has? h "const void *initial_data;"))
  (check "vtable function pointer with a const-struct-pointer arg"
         (has? h
               "gpu_pipeline_t (*pipeline_create)(const struct gpu_pipeline_desc *desc);"))
  (check "void-returning zero-arg function pointer takes (void)"
         (has? h "gpu_cmd_buf_t (*cmd_buf_begin)(void);"))
  (check "pointer return hugs the star"
         (has? h "void *(*gpu_malloc)(size_t size);"))
  (check "declaration forms do not leak into the header"
         (and (not (has? h "c-struct")) (not (has? h "c-handle")))))

(system (string-append "rm -rf " tmp))

(if (= fail-count 0)
    (begin (display "INTROSPECT-TESTS: OK\n") (exit 0))
    (begin (display (string-append "INTROSPECT-TESTS: FAIL ("
                                   (number->string fail-count) ")\n"))
           (exit 1)))
