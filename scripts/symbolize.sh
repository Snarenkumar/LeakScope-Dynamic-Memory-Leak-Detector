#!/bin/sh
# scripts/symbolize.sh: translates stack frame addresses to function names and file:line
EXE="$1"

if [ -z "$EXE" ]; then
    echo "Usage: $0 <binary-path>" >&2
    exit 1
fi

while IFS= read -r line || [ -n "$line" ]; do
    echo "$line"
    case "$line" in
        *"frame ["*)
            # Extract the raw hexadecimal address from the frame line
            addr=$(echo "$line" | grep -oE '0x[0-9a-fA-F]+' | head -n 1)
            if [ -n "$addr" ] && [ -f "$EXE" ]; then
                # Resolve using addr2line with demangling (-C) and function names (-f)
                sym=$(addr2line -e "$EXE" -f -C "$addr" 2>/dev/null)
                func=$(echo "$sym" | head -n 1)
                loc=$(echo "$sym" | sed -n '2p')
                # If resolved to a real symbol (not ??), print symbolized trace
                if [ "$func" != "??" ] && [ -n "$func" ]; then
                    echo "       ↳ $func ($loc)"
                fi
            fi
            ;;
    esac
done
