#!/bin/bash

INPUT_FILE="input.txt"
OUTPUT_FILE="output.txt"

# v 뒤의 숫자만 추출해서 output 파일에 저장
grep -o "v[0-9]\+" "$INPUT_FILE" | sed 's/v//' > "$OUTPUT_FILE"

echo "[*] Extracted values saved to $OUTPUT_FILE"

# 정렬 및 중복 제거
SORTED="sorted.txt"
sort -n "$OUTPUT_FILE" | uniq > "$SORTED"
echo "[*] Sorted unique values saved to $SORTED"

echo "[*] Checking completeness..."
for i in $(seq 0 $(($1 - 2))); do
    grep -q "^$i$" "$SORTED"
    if [ $? -ne 0 ]; then
        echo "Missing value: $i"
    fi
done

echo "[*] Done."
