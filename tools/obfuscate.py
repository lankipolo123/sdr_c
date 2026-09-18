#!/usr/bin/env python3
"""
Obfuscates C string literals for a "protected" build: XOR-encodes each
literal into a byte array, decoded at runtime via obf_decode(). Reads
files from src/, writes transformed copies to build_obf/src/ - never
touches the original readable source, so normal development keeps
working against plain, greppable code. Run this, then compile the
protected exe FROM build_obf/src/ instead of src/.

Skips (left as plain text, reported at the end for manual review):
  - anything inside // or /* */ comments, or '...' char literals
  - anything on a #preprocessor line (handled by hand instead - see
    connection.c's TRANSIT_DLL_PATH, the only #define with a string
    value in this codebase)
  - any string literal inside one of the named static lookup-table
    array initializers listed in KNOWN_ARRAY_NAMES below (matched by
    the array's own NAME, found by walking back from its '=', not by
    line number - line numbers drift every time main.c changes above
    them, which silently broke a line-number-based version of this
    exclusion once already) - those need hand conversion to a runtime-
    filled array since C requires a compile-time constant for a static
    initializer, which a decode function call isn't.
  - any OTHER string literal at brace-depth 0 (file scope) not inside
    a known array - reported as "FILE SCOPE - needs manual review" so
    a genuinely new case never gets silently mishandled.

Usage: python3 tools/obfuscate.py
"""
import os

SRC_DIR = os.path.join(os.path.dirname(__file__), "..", "src")
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "build_obf", "src")
KEY = 0x5A

# Names of static string-array initializers that need hand conversion
# instead of automatic transformation - see the module docstring. Add
# a name here (and hand-convert its build_obf/src output afterward) if
# a future edit introduces a new static array of string literals.
KNOWN_ARRAY_NAMES = {
    "PARITY_LABELS", "LEVEL_LABELS", "row_select_items", "row_lbl_text",
    "MODE_NAMES",
}
# (filename, 1-based line) of #define lines with a string value,
# handled by hand instead of automatically.
DEFINE_EXCLUDE_LINES = {
    "connection.c": [5],
}


def decode_one_segment(body, out):
    """Appends body's real bytes (escapes resolved) to out."""
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c == '\\' and i + 1 < n:
            nc = body[i + 1]
            if nc == 'n':
                out.append(0x0A); i += 2
            elif nc == 't':
                out.append(0x09); i += 2
            elif nc == 'r':
                out.append(0x0D); i += 2
            elif nc == '\\':
                out.append(0x5C); i += 2
            elif nc == '"':
                out.append(0x22); i += 2
            elif nc == '0':
                out.append(0x00); i += 2
            elif nc == 'x':
                j = i + 2
                hexdigits = ''
                while j < n and body[j] in '0123456789abcdefABCDEF' and len(hexdigits) < 2:
                    hexdigits += body[j]
                    j += 1
                out.append(int(hexdigits, 16) if hexdigits else 0)
                i = j
            else:
                out.append(ord(nc)); i += 2
        else:
            out.append(ord(c))
            i += 1


def decode_c_string(raw):
    """raw is one or more adjacent quoted segments (C string-literal
    concatenation - "a" "b" - merged by scan() into one span already).
    Returns the real bytes the whole thing represents (escapes
    resolved, quotes/whitespace/comments between segments dropped)."""
    out = bytearray()
    i = 0
    n = len(raw)
    while i < n:
        if raw[i] != '"':
            i += 1
            continue
        j = i + 1
        while j < n:
            if raw[j] == '\\':
                j += 2
                continue
            if raw[j] == '"':
                break
            j += 1
        decode_one_segment(raw[i + 1:j], out)
        i = j + 1
    return bytes(out)


def scan(text):
    """Returns a list of (start, end, brace_depth_after_open) for each
    string literal in `text` that is real code (not in a comment/char
    literal), plus the byte offset of each literal's enclosing line
    start (for the preprocessor-line check) and the position right
    after the nearest still-open '{' at that point (insertion point
    for hoisted decls, or -1 if at file scope)."""
    i = 0
    n = len(text)
    brace_stack = []  # positions right after each open '{'
    literals = []
    line_start = 0
    while i < n:
        c = text[i]
        if c == '\n':
            line_start = i + 1
            i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            i = j if j != -1 else n
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            i = (j + 2) if j != -1 else n
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == "'":
                    j += 1
                    break
                j += 1
            i = j
            continue
        if c == '{':
            # An initializer-list brace ('= { ... }') is NOT a valid
            # place to hoist a declaration statement into - unlike a
            # code block's brace (function/if/for/while/switch body),
            # which always is. Detected by looking back past whitespace
            # for a preceding '=' (this codebase has no compound
            # literals - '(Type){...}' - confirmed by grep, so this
            # simple check is enough; a struct-typed static/global with
            # a brace initializer, e.g. `static const struct {...} x =
            # {...};`, still gets this right since it's the '='-preceded
            # closing brace that matters, and the TYPE's own '{...}'
            # (no preceding '=') stays a normal, uninvolved code-style
            # brace with no literals inside it in practice here).
            k = i - 1
            while k >= 0 and text[k] in ' \t\r\n':
                k -= 1
            is_initializer = k >= 0 and text[k] == '='
            var_name = None
            if is_initializer:
                # Identify WHICH array this is by name, not by line
                # number - line numbers drift every time main.c grows or
                # shrinks above this point, which silently broke this
                # exclusion list once already (see git history). Walk
                # back past '=', optional '[...]' (array size), and
                # whitespace to the identifier token right before them.
                m = k - 1
                while m >= 0 and text[m] in ' \t\r\n':
                    m -= 1
                if m >= 0 and text[m] == ']':
                    depth = 1
                    m -= 1
                    while m >= 0 and depth > 0:
                        if text[m] == ']':
                            depth += 1
                        elif text[m] == '[':
                            depth -= 1
                        m -= 1
                    while m >= 0 and text[m] in ' \t\r\n':
                        m -= 1
                name_end = m + 1
                while m >= 0 and (text[m].isalnum() or text[m] == '_'):
                    m -= 1
                name_start = m + 1
                if name_start < name_end:
                    var_name = text[name_start:name_end]
            brace_stack.append((i + 1, is_initializer, var_name))
            i += 1
            continue
        if c == '}':
            if brace_stack:
                brace_stack.pop()
            i += 1
            continue
        if c == '"':
            start = i
            j = i + 1
            while j < n:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                j += 1
            end = j
            # C concatenates adjacent string literals ("a" "b" is one
            # literal, "ab") - real usage in this codebase, always split
            # across lines for a long message. Keep merging forward past
            # whitespace/comments as long as another literal follows, so
            # the whole run becomes ONE logical literal (one decode call)
            # instead of N separate calls juxtaposed with no operator
            # between them, which isn't valid C.
            while True:
                k = end
                while k < n:
                    if text[k] in ' \t\r\n':
                        k += 1
                        continue
                    if text[k] == '/' and k + 1 < n and text[k + 1] == '/':
                        nl = text.find('\n', k)
                        k = nl if nl != -1 else n
                        continue
                    if text[k] == '/' and k + 1 < n and text[k + 1] == '*':
                        ce = text.find('*/', k + 2)
                        k = (ce + 2) if ce != -1 else n
                        continue
                    break
                if k < n and text[k] == '"':
                    j = k + 1
                    while j < n:
                        if text[j] == '\\':
                            j += 2
                            continue
                        if text[j] == '"':
                            j += 1
                            break
                        j += 1
                    end = j
                    continue
                break
            # Walk up past any initializer-list frames to the nearest
            # real code-block brace - a literal inside `= { ... }` still
            # needs ITS declaration hoisted into the enclosing function's
            # code block, not into the initializer list itself. Also
            # note the nearest enclosing initializer's array name (None
            # if this literal isn't inside one at all).
            insert_at = -1
            array_name = None
            for pos, is_init, name in reversed(brace_stack):
                if is_init and array_name is None:
                    array_name = name
                if not is_init:
                    insert_at = pos
                    break
            literals.append((start, end, insert_at, line_start, array_name))
            i = end
            continue
        i += 1
    return literals


def transform_file(fname, text):
    literals = scan(text)
    define_lines_excl = set(DEFINE_EXCLUDE_LINES.get(fname, []))

    # Work back-to-front so earlier offsets stay valid as we edit.
    literals.sort(key=lambda t: t[0])

    skipped = []
    edits = []  # (start, end, replacement_text)
    hoist_by_insert_point = {}  # insert_at -> list of decl lines
    counter = [0]

    for start, end, insert_at, line_start, array_name in literals:
        raw = text[start:end]
        if raw == '""':
            continue  # nothing to hide in an empty string
        line_no = text.count('\n', 0, start) + 1

        # Preprocessor line (e.g. #define X "...") - only expected
        # case is connection.c's TRANSIT_DLL_PATH, handled by hand.
        line_text_before = text[line_start:start]
        if line_text_before.lstrip().startswith('#'):
            skipped.append((fname, line_no, raw, "preprocessor line"))
            continue

        # Checked unconditionally (BEFORE the insert_at==-1 branch, not
        # only inside it) - a known static array's own '= {' brace is
        # an initializer-list frame, so scan() walks past it and finds
        # the ENCLOSING function's code-block brace for insert_at
        # (needed for the general FILE-SCOPE-only check below to not
        # misfire on a local `static const char *const x[] = {...};`
        # inside a function) - but that walk-past doesn't make hoisting
        # into it safe: the array itself is still `static`, so its own
        # initializer must stay a compile-time constant either way.
        if array_name in KNOWN_ARRAY_NAMES:
            skipped.append((fname, line_no, raw, "static array initializer (hand-converted)"))
            continue

        if insert_at == -1:
            skipped.append((fname, line_no, raw, "FILE SCOPE - needs manual review"))
            continue

        real_bytes = decode_c_string(raw)
        if len(real_bytes) == 0:
            continue

        counter[0] += 1
        uid = f"{os.path.splitext(fname)[0].replace('.', '_')}_{counter[0]}"
        enc = bytes(b ^ KEY for b in real_bytes)
        enc_literal = ",".join(str(b) for b in enc)
        decl = (
            f"static const unsigned char OBF_E_{uid}[] = {{{enc_literal}}}; "
            f"static char OBF_B_{uid}[{len(real_bytes) + 1}];"
        )
        hoist_by_insert_point.setdefault(insert_at, []).append(decl)
        call = f"obf_decode(OBF_B_{uid}, OBF_E_{uid}, sizeof(OBF_E_{uid}))"
        edits.append((start, end, call))

    # Both literal replacements (start != end, real span in the
    # ORIGINAL text) and hoisted-decl insertions (start == end, a pure
    # insertion point) are keyed to offsets in the ORIGINAL text. Apply
    # them together in ONE back-to-front pass so an earlier edit never
    # invalidates a later (smaller-offset) one still expressed in
    # original-text coordinates - doing this as two separate passes
    # (literals, then insertions) was wrong: an insertion point that
    # falls after an already-applied literal edit would land at the
    # pre-edit offset in the post-edit string, corrupting the output.
    combined = list(edits)
    for insert_at, decls in hoist_by_insert_point.items():
        combined.append((insert_at, insert_at, " " + " ".join(decls)))

    out = text
    for start, end, repl in sorted(combined, key=lambda e: -e[0]):
        out = out[:start] + repl + out[end:]

    if edits:
        out = '#include "obf_runtime.h"\n' + out

    return out, skipped


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(os.path.dirname(__file__), "obf_runtime.h"), 'r', encoding='utf-8') as f:
        runtime_h = f.read()
    with open(os.path.join(OUT_DIR, "obf_runtime.h"), 'w', encoding='utf-8') as f:
        f.write(runtime_h)
    all_skipped = []
    for fname in sorted(os.listdir(SRC_DIR)):
        if fname in ("app.ico", "app.manifest", "app.rc"):
            with open(os.path.join(SRC_DIR, fname), 'rb') as f:
                data = f.read()
            with open(os.path.join(OUT_DIR, fname), 'wb') as f:
                f.write(data)
            continue
        if not fname.endswith(('.c', '.h')):
            continue
        src_path = os.path.join(SRC_DIR, fname)
        with open(src_path, 'r', encoding='utf-8') as f:
            text = f.read()
        if fname.endswith('.c'):
            out, skipped = transform_file(fname, text)
            all_skipped.extend(skipped)
        else:
            out = text  # headers untouched (no risky literals found in them)
        out_path = os.path.join(OUT_DIR, fname)
        with open(out_path, 'w', encoding='utf-8') as f:
            f.write(out)
    print(f"Wrote transformed sources to {OUT_DIR}")
    print(f"\n{len(all_skipped)} literal(s) left as plain text (handled by hand or reviewed):")
    for fname, line_no, raw, reason in all_skipped:
        print(f"  {fname}:{line_no}: {raw} -- {reason}")


if __name__ == "__main__":
    main()
