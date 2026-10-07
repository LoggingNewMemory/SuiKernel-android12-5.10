#!/bin/bash

# Default to checking all .c files if no arguments provided
FILES="${@:-*.c}"
has_error=0

# Create a temporary python script for structural syntax checks
cat << 'EOF' > /tmp/check_syntax.py
import sys

def check_brackets(filename):
    try:
        with open(filename, 'r') as f:
            text = f.read()
    except Exception as e:
        print(f"  Error reading file: {e}")
        return 1

    stack = []
    line_num = 1
    in_string = False
    in_char = False
    in_comment_line = False
    in_comment_block = False

    i = 0
    while i < len(text):
        c = text[i]
        
        if c == '\n':
            line_num += 1
            in_comment_line = False
            i += 1
            continue
            
        if in_comment_line:
            i += 1
            continue
            
        if in_comment_block:
            if c == '*' and i + 1 < len(text) and text[i+1] == '/':
                in_comment_block = False
                i += 2
            else:
                i += 1
            continue
            
        if in_string:
            if c == '\\':
                if i + 1 < len(text) and text[i+1] == '\n':
                    line_num += 1
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue
            
        if in_char:
            if c == '\\':
                if i + 1 < len(text) and text[i+1] == '\n':
                    line_num += 1
                i += 2
                continue
            if c == "'":
                in_char = False
            i += 1
            continue
            
        if c == '/' and i + 1 < len(text):
            if text[i+1] == '/':
                in_comment_line = True
                i += 2
                continue
            if text[i+1] == '*':
                in_comment_block = True
                i += 2
                continue
                
        if c == '"':
            in_string = True
        elif c == "'":
            in_char = True
        elif c in '{[(':
            stack.append((c, line_num))
        elif c in '}])':
            if not stack:
                print(f"  Error: Unmatched closing '{c}' at line {line_num}")
                return 1
            top, top_line = stack.pop()
            if (top == '{' and c != '}') or \
               (top == '[' and c != ']') or \
               (top == '(' and c != ')'):
                print(f"  Error: Mismatched brackets. Expected closing for '{top}' (from line {top_line}), but found '{c}' at line {line_num}")
                return 1
        i += 1
        
    if in_string:
        print("  Error: Unclosed string literal")
        return 1
    if in_comment_block:
        print("  Error: Unclosed block comment")
        return 1
    if stack:
        top, top_line = stack[-1]
        print(f"  Error: Unclosed '{top}' from line {top_line}")
        return 1
        
    return 0

sys.exit(check_brackets(sys.argv[1]))
EOF

# Create a temporary awk script to perform the heuristic check
cat << 'EOF' > /tmp/check_decl.awk
BEGIN { in_block = 0; stmt_seen = 0; err_count = 0 }
{
    line = $0
    # Trim leading whitespace for easier matching
    sub(/^[ \t]+/, "", line)
    
    # Ignore empty lines, braces, comments, and preprocessor directives
    if (line == "" || line ~ /^\/\// || line ~ /^\/\*/ || line ~ /^#/) next
    # Ignore block comment continuations
    if (line ~ /^\*/ && line !~ /;/) next
    
    # Hack Kobo V3: Buang semua string dan inisialisasi array sebaris!
    clean_line = line
    gsub(/"[^"]*"/, "", clean_line)
    while (match(clean_line, /{[^}]*}/)) {
        clean_line = substr(clean_line, 1, RSTART-1) substr(clean_line, RSTART+RLENGTH)
    }
    
    # Check if the line is a variable declaration
    is_decl = 0
    # Kobo tambahin loff_t biar Inaho gak kena tilang!
    if (line ~ /^(static|const|extern|volatile|register|unsigned|signed|inline|int|char|void|long|short|float|double|struct|union|enum|bool)[ \t*]/) {
        is_decl = 1
    } else if (line ~ /^([a-zA-Z0-9_]+_t|u8|u16|u32|u64|s8|s16|s32|s64|mutex|loff_t)[ \t*]/) {
        is_decl = 1
    }
    
    if (in_block > 0) {
        if (is_decl && stmt_seen) {
            err_count++
            print "  Line " NR ": " $0
        } else if (!is_decl) {
            # Only mark as statement if it ends with a semicolon and is not a control flow keyword
            if (line !~ /^(return|if|for|while|switch|break|continue|goto|case|default)[ \t(]/ && line ~ /;/) {
                stmt_seen = 1
            }
        }
    }
    
    # Update scope tracking pakai clean_line yang udah bersih dari {0}
    if (clean_line ~ /{/) {
        in_block++
        stmt_seen = 0 # New block, boleh deklarasi variabel lagi dari awal
    }
    if (clean_line ~ /}/) {
        in_block--
        if (in_block < 0) in_block = 0
        stmt_seen = 1 # Abis nutup block, block di luarnya udah pasti kehitung melihat statement
    }
}
END { exit (err_count > 0 ? 1 : 0) }
EOF

# Run the check on each file
for file in $FILES; do
    if [ ! -f "$file" ]; then
        continue
    fi
    
    file_has_error=0
    error_output=""
    
    # Check structural syntax
    syntax_output=$(python3 /tmp/check_syntax.py "$file")
    if [ $? -ne 0 ]; then
        file_has_error=1
        error_output+="$syntax_output\n"
    fi
    
    # Check C90 mixing declarations and code
    decl_output=$(awk -f /tmp/check_decl.awk "$file")
    if [ $? -ne 0 ]; then
        file_has_error=1
        error_output+="  Error: ISO C90 forbids mixing declarations and code\n"
        error_output+="$decl_output\n"
    fi
    
    if [ $file_has_error -eq 0 ]; then
        echo -e "Checking [$file] \e[32m✓\e[0m"
    else
        echo -e "Checking [$file] \e[31m✗\e[0m"
        echo -e "$error_output" | sed '/^$/d'
        has_error=1
    fi
done

# Cleanup
rm -f /tmp/check_decl.awk /tmp/check_syntax.py
exit $has_error