#!/bin/bash
cd "$(dirname "$0")/../.." && g++ -std=c++20 -O2 -Wall -Wextra -pthread tools/pf/psim.cpp policies/*.cpp -o bin/psim
