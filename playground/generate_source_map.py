#!/usr/bin/env python3
"""
CLing Source Map Generator

This script generates a source map file that maps lines in the generated C++ code
back to the original CLing source code. This enables proper debugging in VS Code.
"""

import re
import json
import sys
from pathlib import Path

def generate_source_map(cling_file, cpp_file, output_file):
    """
    Generate a source map by comparing the original CLing file with generated C++.
    """

    # Read both files
    with open(cling_file, 'r') as f:
        cling_lines = f.readlines()

    with open(cpp_file, 'r') as f:
        cpp_lines = f.readlines()

    # Create line mapping
    line_mapping = {}

    # Simple line-by-line mapping (can be enhanced)
    for cpp_line_num, cpp_line in enumerate(cpp_lines, 1):
        # Look for corresponding line in CLing source
        for cling_line_num, cling_line in enumerate(cling_lines, 1):
            # Remove transformations and compare core content
            cpp_normalized = normalize_line(cpp_line)
            cling_normalized = normalize_line(cling_line)

            if cpp_normalized and cling_normalized and \
               similarity_score(cpp_normalized, cling_normalized) > 0.8:
                line_mapping[cpp_line_num] = cling_line_num
                break

        # If no match found, try to find closest match
        if cpp_line_num not in line_mapping:
            closest_match = find_closest_match(cpp_line, cling_lines)
            if closest_match:
                line_mapping[cpp_line_num] = closest_match

    # Generate source map in JSON format
    source_map = {
        "version": 3,
        "file": str(Path(cpp_file).name),
        "sourceRoot": "",
        "sources": [str(Path(cling_file).name)],
        "names": [],
        "mappings": encode_mappings(line_mapping),
        "lineMapping": line_mapping  # Custom field for easy debugging
    }

    # Write source map
    with open(output_file, 'w') as f:
        json.dump(source_map, f, indent=2)

    print(f"Source map generated: {output_file}")
    print(f"Mapped {len(line_mapping)} lines")

def normalize_line(line):
    """Normalize a line by removing smart pointer transformations and whitespace."""
    # Remove smart pointer wrapper
    normalized = re.sub(r'std::shared_ptr<(\w+)>', r'\1', line)
    normalized = re.sub(r'std::make_shared<(\w+)>', r'new \1', normalized)

    # Remove extra whitespace
    normalized = re.sub(r'\s+', ' ', normalized.strip())

    return normalized

def similarity_score(line1, line2):
    """Calculate similarity score between two normalized lines."""
    if not line1 or not line2:
        return 0.0

    # Simple similarity based on common words
    words1 = set(line1.split())
    words2 = set(line2.split())

    if not words1 or not words2:
        return 0.0

    intersection = words1.intersection(words2)
    union = words1.union(words2)

    return len(intersection) / len(union)

def find_closest_match(cpp_line, cling_lines):
    """Find the closest matching line in CLing source."""
    cpp_normalized = normalize_line(cpp_line)
    if not cpp_normalized:
        return None

    best_score = 0.0
    best_match = None

    for line_num, cling_line in enumerate(cling_lines, 1):
        cling_normalized = normalize_line(cling_line)
        if cling_normalized:
            score = similarity_score(cpp_normalized, cling_normalized)
            if score > best_score:
                best_score = score
                best_match = line_num

    return best_match if best_score > 0.3 else None

def encode_mappings(line_mapping):
    """Encode line mappings in source map format (simplified)."""
    # This is a simplified version - real source maps use VLQ encoding
    return ";".join(f"{cpp_line}:{cling_line}" for cpp_line, cling_line in sorted(line_mapping.items()))

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: python generate_source_map.py <cling_file> <cpp_file> <output_file>")
        sys.exit(1)

    cling_file, cpp_file, output_file = sys.argv[1:4]
    generate_source_map(cling_file, cpp_file, output_file)