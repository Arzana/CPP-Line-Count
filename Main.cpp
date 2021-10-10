#define _CRT_SECURE_NO_WARNINGS
#pragma warning (disable:4309)

#include <fstream>		// ifstream
#include <cstdio>		// fopen
#include <chrono>		// timers
#include <conio.h>		// _getch
#include <intrin.h>		// SIMD

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>		// Windows file API
#endif

/* Block size should be the memory page size (4KiB). */
constexpr size_t BLOCK_SIZE = 1 << 12;

#if defined(_____LP64_____) || defined(_WIN64)
#define popcnt_buildin _mm_popcnt_u64
#define popcnt_fallback popcnt_fallback64
using word = uint64_t;

#ifdef _DEBUG
#define MODE_STR	"64-Bit|DEBUG"
#else
#define MODE_STR	"64-Bit|RELEASE"
#endif

/* Used only by page read fallback on 64-bit mode. */
size_t popcnt_fallback64(uint64_t x)
{
	x = x - ((x >> 1) & 0x5555555555555555ULL);
	x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
	return (((x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL) * 0x0101010101010101ULL) >> 56;
}
#else
#define popcnt_buildin _mm_popcnt_u32
#define popcnt_fallback popcnt_fallback32
using word = uint32_t;

#ifdef _DEBUG
#define MODE_STR	"32-Bit|DEBUG"
#else
#define MODE_STR	"32-Bit|RELEASE"
#endif
#endif

/* Used by SSE and AVX fallbacks. */
size_t popcnt_fallback32(uint32_t x)
{
	x = x - ((x >> 1) & 0x55555555);
	x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
	return (((x + (x >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

/*
This function represents the elegant C++ STL way of solving the issue.
It is by far the shortest, easiest to read, and the safest as ifstream closes itself.
It is however the slowest on debug mode and the second slowest on release mode.
*/
size_t line_count_ifstream(const char *file_name)
{
	std::ifstream hFile{ file_name };
	return static_cast<size_t>(std::count(std::istreambuf_iterator<char>(hFile), std::istreambuf_iterator<char>(), '\n'));
}

/*
This function represents the default C way of solving the issue.
It is pretty short and relatively easy to read, the user is required to close the file handle themselves.
This is the slowest on release mode and the second slowest on debug mode.
*/
size_t line_count_getc(const char *file_name)
{
	FILE *hFile = fopen(file_name, "rb");

	size_t result = 0;
	for (int c = getc(hFile); c != EOF; c = getc(hFile)) 
		result += c == '\n';

	fclose(hFile);
	return result;
}

/*
The idea of this function is to be the fastest portable function to solve the issue.
It reads from the file one memory page at a time for optimal buffering.
Instead of checking one character at a time, it instead packs them together into a word (32-bit or 64-bit).
These can be checked in parallel by some binary code, allowing for 4-8 bytes checked at one time (depending on the architecture).
This algorithm in called SWAR (SIMD Within A Register).
*/
size_t line_count_block_read(const char *file_name)
{
	constexpr size_t BLOCK_SIZE_WORD = BLOCK_SIZE / sizeof(word);

	FILE *hFile = fopen(file_name, "rb");

	word memory[BLOCK_SIZE_WORD];
	char *buffer = reinterpret_cast<char *>(memory);

	size_t result = 0;
	size_t bytes_read;
	while ((bytes_read = fread(buffer, sizeof(char), BLOCK_SIZE, hFile)) > 0) {
		memset(buffer + bytes_read, 0, BLOCK_SIZE - bytes_read);
		for (size_t i = 0; i < BLOCK_SIZE_WORD; i++) {
			constexpr word MASK_NL = static_cast<word>(0x0a0a0a0a0a0a0a0aLL);
			constexpr word MASK_SUB = static_cast<word>(0x0101010101010101LL);
			constexpr word MASK_AND = static_cast<word>(0x8080808080808080LL);

			const word data = memory[i] ^ MASK_NL;
			result += popcnt_fallback((data - MASK_SUB) & ~data & MASK_AND);
		}
	}

	fclose(hFile);
	return result;
}

/*
This function is the same as the block read, but instead of using the C-API it uses the OS-API.
*/
#ifdef _WIN32
size_t line_count_osAPI(const char *file_name)
{
	constexpr size_t BLOCK_SIZE_WORD = BLOCK_SIZE / sizeof(word);

	HANDLE hFile = CreateFileA(file_name, FILE_READ_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

	word memory[BLOCK_SIZE_WORD];
	char *buffer = reinterpret_cast<char *>(memory);

	size_t result = 0;
	for (DWORD bytes_read; ReadFile(hFile, memory, BLOCK_SIZE, &bytes_read, nullptr) && bytes_read;) {
		memset(buffer + bytes_read, 0, BLOCK_SIZE - bytes_read);
		for (size_t i = 0; i < BLOCK_SIZE_WORD; i++) {
			constexpr word MASK_NL = static_cast<word>(0x0a0a0a0a0a0a0a0aLL);
			constexpr word MASK_SUB = static_cast<word>(0x0101010101010101LL);
			constexpr word MASK_AND = static_cast<word>(0x8080808080808080LL);

			const word data = memory[i] ^ MASK_NL;
			result += popcnt_fallback((data - MASK_SUB) & ~data & MASK_AND);
		}
	}

	CloseHandle(hFile);
	return result;
}
#endif

/*
The SIMD family of functions in this file all do the same thing as the block read function.
They are however optimized for CPUs that support SIMD.

This function relies SSE2 and POPCNT to be supported by the CPU.
This can check 16 character at one time and uses as faster popcnt.
*/
size_t line_count_sse(const char *file_name)
{
	constexpr size_t BLOCK_SIZE_AVX = BLOCK_SIZE / sizeof(__m128i);

	const __m128i MASK_NL = _mm_set1_epi8('\n');
	const __m128i MASK_SUB = _mm_set1_epi8(0x01);
	const __m128i MASK_AND = _mm_set1_epi8(0x80);

	FILE *hFile = fopen(file_name, "rb");

	__m128i memory[BLOCK_SIZE_AVX];
	char *buffer = reinterpret_cast<char *>(memory);

	size_t result = 0;
	size_t bytes_read;
	while ((bytes_read = fread(buffer, sizeof(char), BLOCK_SIZE, hFile)) > 0) {
		memset(buffer + bytes_read, 0, BLOCK_SIZE - bytes_read);
		for (size_t i = 0; i < BLOCK_SIZE_AVX; i++) {
			__m128i data = _mm_xor_si128(memory[i], MASK_NL);
			data = _mm_and_si128(_mm_sub_epi8(data, MASK_SUB), _mm_andnot_si128(data, MASK_AND));
			result += _mm_popcnt_u32(_mm_movemask_epi8(data));
		}
	}

	fclose(hFile);
	return result;
}

/*
The SIMD family of functions in this file all do the same thing as the block read function.
They are however optimized for CPUs that support SIMD.

This function relies SSE2 to be supported by the CPU.
This can check 16 character at one time.
*/
size_t line_count_sse_no_buildin_popcnt(const char *file_name)
{
	constexpr size_t BLOCK_SIZE_AVX = BLOCK_SIZE / sizeof(__m128i);

	const __m128i MASK_NL = _mm_set1_epi8('\n');
	const __m128i MASK_SUB = _mm_set1_epi8(0x01);
	const __m128i MASK_AND = _mm_set1_epi8(0x80);

	FILE *hFile = fopen(file_name, "rb");

	__m128i memory[BLOCK_SIZE_AVX];
	char *buffer = reinterpret_cast<char *>(memory);

	size_t result = 0;
	size_t bytes_read;
	while ((bytes_read = fread(buffer, sizeof(char), BLOCK_SIZE, hFile)) > 0) {
		memset(buffer + bytes_read, 0, BLOCK_SIZE - bytes_read);
		for (size_t i = 0; i < BLOCK_SIZE_AVX; i++) {
			__m128i data = _mm_xor_si128(memory[i], MASK_NL);
			data = _mm_and_si128(_mm_sub_epi8(data, MASK_SUB), _mm_andnot_si128(data, MASK_AND));
			result += popcnt_fallback32(_mm_movemask_epi8(data));
		}
	}

	fclose(hFile);
	return result;
}

/*
The SIMD family of functions in this file all do the same thing as the block read function.
They are however optimized for CPUs that support SIMD.

This function relies AVX2 and POPCNT to be supported by the CPU.
This can check 32 character at one time and uses as faster popcnt.
This function should be the fastest.
*/
size_t line_count_avx(const char *file_name)
{
	constexpr size_t BLOCK_SIZE_AVX = BLOCK_SIZE / sizeof(__m256i);

	const __m256i MASK_NL = _mm256_set1_epi8('\n');
	const __m256i MASK_SUB = _mm256_set1_epi8(0x01);
	const __m256i MASK_AND = _mm256_set1_epi8(0x80);

	FILE *hFile = fopen(file_name, "rb");

	__m256i memory[BLOCK_SIZE_AVX];
	char *buffer = reinterpret_cast<char *>(memory);

	size_t result = 0;
	size_t bytes_read;
	while ((bytes_read = fread(buffer, sizeof(char), BLOCK_SIZE, hFile)) > 0) {
		memset(buffer + bytes_read, 0, BLOCK_SIZE - bytes_read);
		for (size_t i = 0; i < BLOCK_SIZE_AVX; i++) {
			__m256i data = _mm256_xor_si256(memory[i], MASK_NL);
			data = _mm256_and_si256(_mm256_sub_epi8(data, MASK_SUB), _mm256_andnot_si256(data, MASK_AND));
			result += _mm_popcnt_u32(_mm256_movemask_epi8(data));
		}
	}

	fclose(hFile);
	return result;
}

/*
The SIMD family of functions in this file all do the same thing as the block read function.
They are however optimized for CPUs that support SIMD.

This function relies AVX2 to be supported by the CPU.
This can check 32 character at one time.
*/
size_t line_count_avx_no_buildin_popcnt(const char *file_name)
{
	constexpr size_t BLOCK_SIZE_AVX = BLOCK_SIZE / sizeof(__m256i);

	const __m256i MASK_NL = _mm256_set1_epi8('\n');
	const __m256i MASK_SUB = _mm256_set1_epi8(0x01);
	const __m256i MASK_AND = _mm256_set1_epi8(0x80);

	FILE *hFile = fopen(file_name, "rb");

	__m256i memory[BLOCK_SIZE_AVX];
	char *buffer = reinterpret_cast<char *>(memory);

	size_t result = 0;
	size_t bytes_read;
	while ((bytes_read = fread(buffer, sizeof(char), BLOCK_SIZE, hFile)) > 0) {
		memset(buffer + bytes_read, 0, BLOCK_SIZE - bytes_read);
		for (size_t i = 0; i < BLOCK_SIZE_AVX; i++) {
			__m256i data = _mm256_xor_si256(memory[i], MASK_NL);
			data = _mm256_and_si256(_mm256_sub_epi8(data, MASK_SUB), _mm256_andnot_si256(data, MASK_AND));
			result += popcnt_fallback32(_mm256_movemask_epi8(data));
		}
	}

	fclose(hFile);
	return result;
}

void run_timed_test(const char *file_name, uint64_t file_size, const char *function_name, size_t(*function)(const char *))
{
	const std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();
	const size_t line_cnt = function(file_name);
	const std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now();

	const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
	const float mb = file_size / 1000000.0f;

	printf("%-30s took %03lldms to count %zu lines (%.2f MB/s).\n", function_name, ms, line_cnt, mb / (ms * 0.001f));
}

uint64_t get_file_size(const char *file_name)
{
	FILE *hfile = fopen(file_name, "rb");
	fseek(hfile, 0, SEEK_END);
	const long size = ftell(hfile);
	fclose(hfile);
	return static_cast<uint64_t>(size);
}

int main(int argc, char **argv)
{
#define RUN_TIMED_TEST(func, name)	run_timed_test(file_name, file_size, name, func)

	if (argc != 2) 
		return EXIT_FAILURE;

	const char *file_name = argv[1];
	const uint64_t file_size = get_file_size(file_name);

	printf("Running performance tests on " MODE_STR " mode.\n");

#ifdef _WIN32
	SYSTEM_INFO system_info;
	GetSystemInfo(&system_info);
	if (system_info.dwPageSize != BLOCK_SIZE)
		printf("System page size is not equal to %zu, consider setting BLOCK_SIZE = %u.\n", BLOCK_SIZE, system_info.dwPageSize);
#endif

	/* Check for CPU support. */
	int registers[4];
	__cpuid(registers, 1);
	const bool popcnt_supported = registers[2] & 1 << 22;
	const bool sse2_supported = registers[3] & 1 << 25;
	__cpuidex(registers, 7, 0);
	const bool avx2_supported = registers[1] & 1 << 4;

	if (popcnt_supported) {
		if (avx2_supported) RUN_TIMED_TEST(line_count_avx, "block read (AVX2 & POPCNT)");
		if (sse2_supported) RUN_TIMED_TEST(line_count_sse, "block read (SSE2 & POPCNT)");
	}
	else puts("CPU doesn't support POPCNT.\n");

	/* Check for AVX2 support (extended features). */
	if (avx2_supported) RUN_TIMED_TEST(line_count_avx_no_buildin_popcnt, "block read (AVX2)");
	else puts("CPU doesn't support AVX2.\n");

	/* Check for SSE2 support. */
	if (sse2_supported) RUN_TIMED_TEST(line_count_sse_no_buildin_popcnt, "block read (SSE2)");
	else puts("CPU doesn't support SSE2.\n");

	/* Run tests that don't depend on extended instruction sets. */
#ifdef _WIN32
	RUN_TIMED_TEST(line_count_osAPI, "block read (OS API)");
#else
	printf("OS API not supported on this platform.\n");
#endif
	RUN_TIMED_TEST(line_count_block_read, "block read (Generic API)");
	RUN_TIMED_TEST(line_count_ifstream, "std ifstream count");
	RUN_TIMED_TEST(line_count_getc, "getc loop");

	puts("Finishes running performance tests.");
	return EXIT_SUCCESS;
}
