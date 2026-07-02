import os

replacements = {
    '"order_book.h"': '"orderbook/order_book.h"',
    '"matching_engine.h"': '"matching/matching_engine.h"',
    '"exchange.h"': '"exchange/exchange.h"',
    '"types.h"': '"orderbook/types.h"',
    '"risk_engine.h"': '"risk/risk_engine.h"',
    '"spsc_queue.h"': '"utils/spsc_queue.h"',
    '"itch_parser.h"': '"replay/itch_parser.h"',
}

def process_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    
    orig = content
    # For each line that is an include, if it matches, replace it
    lines = content.split('\n')
    for i, line in enumerate(lines):
        if line.strip().startswith('#include'):
            for k, v in replacements.items():
                if k in line:
                    lines[i] = line.replace(k, v)
    
    content = '\n'.join(lines)
        
    if content != orig:
        with open(filepath, 'w') as f:
            f.write(content)
        print(f"Updated {filepath}")

for root, dirs, files in os.walk('.'):
    if 'build' in root or '.git' in root:
        continue
    for file in files:
        if file.endswith('.cpp') or file.endswith('.h'):
            process_file(os.path.join(root, file))

print("Include updates complete phase 2.")
