# CPP-Line-Count
Compares the performance cost of counting the number of lines in a specified file for various C/C++ methods.  
Test results are on i7-6800K with a file containing 14070498 lines on 64-Bit realease mode.

| Method    | Requirements | Time (ms) |
| --------- | ------------ | --------: |
| AVX       | AVX2, POPCNT | 399       |
| AVX       | AVX2         | 418       |
| SSE       | SSE2, POPCNT | 437       |
| SSE       | SSE2         | 460       |
| Block     |              | 521       |
| std count | C++          | 4084      |
| getc      |              | 12862     |
