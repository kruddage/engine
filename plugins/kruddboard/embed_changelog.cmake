# SPDX-License-Identifier: GPL-2.0-or-later
#
# embed_changelog.cmake — bake a text file into a C header as a
# NUL-terminated byte array, so kruddboard's "What's New" tab can render
# CHANGELOG.md with no runtime fetch and no extra ABI surface.
#
# Invoked at build time by plugins/kruddboard/CMakeLists.txt:
#   cmake -DINPUT=<file> -DOUTPUT=<header> -DSYMBOL=<c_identifier> \
#         -P embed_changelog.cmake

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
	message(FATAL_ERROR
		"embed_changelog.cmake: INPUT, OUTPUT and SYMBOL are required")
endif()

# Read the file as a flat hex string, then rewrite each byte into a \xNN
# escape inside one C string literal. Going through HEX sidesteps every
# quoting/escaping hazard that embedding the raw text would hit — quotes,
# backslashes, trailing-newline surprises — and, unlike a { 0x.. } byte
# array, a string literal never narrows on the changelog's non-ASCII bytes
# (em dashes etc.) and is implicitly NUL-terminated for md_parse(). Every
# byte becomes exactly "\xNN", so a hex escape is always followed by a
# backslash, never an extra hex digit that would extend the escape.
file(READ "${INPUT}" _hex HEX)
string(REGEX REPLACE "(..)" "\\\\x\\1" _body "${_hex}")

set(_out "/* SPDX-License-Identifier: GPL-2.0-or-later */\n")
string(APPEND _out
	"/* Generated from CHANGELOG.md by embed_changelog.cmake. Do not edit. */\n")
string(APPEND _out "#ifndef CHANGELOG_DATA_H\n")
string(APPEND _out "#define CHANGELOG_DATA_H\n\n")
string(APPEND _out "static const char ${SYMBOL}[] =\n\t\"${_body}\";\n\n")
string(APPEND _out "#endif /* CHANGELOG_DATA_H */\n")

file(WRITE "${OUTPUT}" "${_out}")
