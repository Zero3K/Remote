#!/usr/bin/env python3
"""
Basic syntax validation for the C++ code changes.
This script checks for common syntax errors that could prevent compilation.
"""

import re
import sys

def validate_cpp_syntax(filepath):
    """Basic syntax validation for C++ code"""
    errors = []
    
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    
    lines = content.split('\n')
    
    # Track brace balance
    brace_count = 0
    paren_count = 0
    bracket_count = 0
    
    # Track if we're in a string or comment
    in_string = False
    in_comment = False
    in_multiline_comment = False
    
    for i, line in enumerate(lines, 1):
        line_stripped = line.strip()
        
        # Skip empty lines
        if not line_stripped:
            continue
            
        # Check for basic syntax patterns
        j = 0
        while j < len(line):
            char = line[j]
            
            # Handle comments
            if not in_string:
                if j < len(line) - 1:
                    if line[j:j+2] == '//':
                        in_comment = True
                        break
                    elif line[j:j+2] == '/*':
                        in_multiline_comment = True
                        j += 2
                        continue
                    elif line[j:j+2] == '*/' and in_multiline_comment:
                        in_multiline_comment = False
                        j += 2
                        continue
            
            if in_comment or in_multiline_comment:
                j += 1
                continue
                
            # Handle strings
            if char == '"' and (j == 0 or line[j-1] != '\\'):
                in_string = not in_string
            elif in_string:
                j += 1
                continue
                
            # Count braces, parentheses, brackets
            if char == '{':
                brace_count += 1
            elif char == '}':
                brace_count -= 1
                if brace_count < 0:
                    errors.append(f"Line {i}: Unmatched closing brace")
            elif char == '(':
                paren_count += 1
            elif char == ')':
                paren_count -= 1
                if paren_count < 0:
                    errors.append(f"Line {i}: Unmatched closing parenthesis")
            elif char == '[':
                bracket_count += 1
            elif char == ']':
                bracket_count -= 1
                if bracket_count < 0:
                    errors.append(f"Line {i}: Unmatched closing bracket")
                    
            j += 1
            
        # Reset comment flag at end of line
        in_comment = False
    
    # Check final balance
    if brace_count != 0:
        errors.append(f"Unbalanced braces: {brace_count} excess opening braces")
    if paren_count != 0:
        errors.append(f"Unbalanced parentheses: {paren_count} excess opening parentheses")
    if bracket_count != 0:
        errors.append(f"Unbalanced brackets: {bracket_count} excess opening brackets")
    
    # Check for basic C++ syntax patterns
    
    # Check for function definitions without proper syntax
    func_pattern = re.compile(r'^\s*\w+\s+\w+\s*\([^)]*\)\s*$')
    for i, line in enumerate(lines, 1):
        if func_pattern.match(line) and not line.strip().endswith(';'):
            # This might be a function definition without opening brace
            if i < len(lines) and not lines[i].strip().startswith('{'):
                errors.append(f"Line {i}: Possible function definition without opening brace")
    
    # Check for missing semicolons (basic check)
    for i, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped and not any(stripped.endswith(x) for x in ['{', '}', ';', ':', '//', '*/', ',']) and \
           not any(stripped.startswith(x) for x in ['#', '//', '/*', '}', 'case ', 'default:']):
            # Check if it looks like a statement that should end with semicolon
            if any(keyword in stripped for keyword in ['return ', 'break', 'continue', '++', '--', '=']):
                if not stripped.endswith('\\'):  # Not a macro continuation
                    errors.append(f"Line {i}: Possible missing semicolon: {stripped}")
    
    return errors

def main():
    filepath = '/home/runner/work/Remote/Remote/main.cpp'
    
    print("Validating C++ syntax...")
    errors = validate_cpp_syntax(filepath)
    
    if errors:
        print(f"Found {len(errors)} potential syntax issues:")
        for error in errors[:10]:  # Show first 10 errors
            print(f"  {error}")
        if len(errors) > 10:
            print(f"  ... and {len(errors) - 10} more issues")
        return 1
    else:
        print("✓ Basic syntax validation passed!")
        return 0

if __name__ == '__main__':
    sys.exit(main())