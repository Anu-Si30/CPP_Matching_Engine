import os
import shutil

moves = [
    ("include/core/types.h", "src/orderbook/types.h"),
    ("include/core/order_book.h", "src/orderbook/order_book.h"),
    ("src/core/order_book.cpp", "src/orderbook/order_book.cpp"),
    
    ("include/core/matching_engine.h", "src/matching/matching_engine.h"),
    ("src/core/matching_engine.cpp", "src/matching/matching_engine.cpp"),
    
    ("include/core/exchange.h", "src/exchange/exchange.h"),
    ("src/core/exchange.cpp", "src/exchange/exchange.cpp"),
    
    ("include/core/risk_engine.h", "src/risk/risk_engine.h"),
    ("include/core/spsc_queue.h", "src/utils/spsc_queue.h"),
    ("include/core/itch_parser.h", "src/replay/itch_parser.h"),
]

for src, dst in moves:
    if os.path.exists(src):
        shutil.move(src, dst)
        print(f"Moved {src} to {dst}")
    else:
        print(f"Missing {src}")

# Move traders
for root, dirs, files in os.walk("include/traders"):
    for file in files:
        src = os.path.join(root, file)
        dst = os.path.join("src/traders", file)
        shutil.move(src, dst)
        print(f"Moved {src} to {dst}")

for root, dirs, files in os.walk("src/traders"):
    for file in files:
        if file.endswith(".cpp"):
            # wait, they are already in src/traders. Let's make sure it doesn't try to move them over themselves
            pass

print("Done moving files.")
