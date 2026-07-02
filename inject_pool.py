import os

def process_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    
    orig = content
    content = content.replace('std::make_shared<Order>', 'global_order_pool.acquire')
    
    # Also inject #include "utils/object_pool.h" into types.h manually
    if filepath.endswith('types.h'):
        if '#include "utils/object_pool.h"' not in content:
            content = content.replace('#include <memory>', '#include <memory>\n#include "utils/object_pool.h"')

    if content != orig:
        with open(filepath, 'w') as f:
            f.write(content)
        print(f"Updated {filepath}")

for root, dirs, files in os.walk('.'):
    if 'build' in root or '.git' in root or 'update_includes.py' in root:
        continue
    for file in files:
        if file.endswith('.cpp') or file.endswith('.h'):
            process_file(os.path.join(root, file))

print("Object pool injection complete.")
