#!/bin/bash

# Default to checking all .c files if no arguments provided
FILES="${@:-*.c}"
has_error=0

# Create a temporary awk script to perform the heuristic check
cat << 'EOF' > /tmp/check_decl.awk
BEGIN { in_block = 0; stmt_seen = 0; err_count = 0 }
/{/ { in_block++; stmt_seen = 0 }
/}/ { in_block--; if (in_block < 0) in_block = 0; stmt_seen = 0 }
in_block > 0 {
    line = $0
    # Trim leading whitespace for easier matching
    sub(/^[ \t]+/, "", line)
    
    # Ignore empty lines, braces, comments, and preprocessor directives
    if (line == "" || line ~ /^{/ || line ~ /^}/ || line ~ /^\/\// || line ~ /^\/\*/ || line ~ /^#/) next
    # Ignore block comment continuations
    if (line ~ /^\*/ && line !~ /;/) next
    
    # Check if the line is a variable declaration
    is_decl = 0
    if (line ~ /^(static|const|extern|volatile|register|unsigned|signed|inline|int|char|void|long|short|float|double|struct|union|enum|bool)[ \t*]/) {
        is_decl = 1
    } else if (line ~ /^([a-zA-Z0-9_]+_t|u8|u16|u32|u64|s8|s16|s32|s64|mutex)[ \t*]/) {
        is_decl = 1
    }
    
    if (is_decl && stmt_seen) {
        err_count++
        print "  Line " NR ": " $0
    } else if (!is_decl) {
        # Only mark as statement if it ends with a semicolon and is not a control flow keyword
        # This ignores labels and multi-line macros
        if (line !~ /^(return|if|for|while|switch|break|continue|goto|case|default)[ \t(]/ && line ~ /;/) {
            stmt_seen = 1
        }
    }
}
END { exit (err_count > 0 ? 1 : 0) }
EOF

# Run the check on each file
for file in $FILES; do
    if [ ! -f "$file" ]; then
        continue
    fi
    
    output=$(awk -f /tmp/check_decl.awk "$file")
    status=$?
    
    if [ $status -eq 0 ]; then
        echo "Checking [$file] ✓"
    else
        echo "Checking [$file] ✗"
        echo "  Error: ISO C90 forbids mixing declarations and code"
        echo "$output"
        has_error=1
    fi
done

# Cleanup
rm -f /tmp/check_decl.awk
exit $has_error
