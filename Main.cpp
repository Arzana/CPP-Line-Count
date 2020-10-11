#define _CRT_SECURE_NO_WARNINGS
#pragma warning (disable:4309)

#include <fstream>		// ifstream
#include <cstdio>		// fopen
#include <chrono>		// timers
#include <conio.h>		// _getch
#include <intrin.h>		// SIMD

#define RUN_TIMED_TEST(func, name)	run_timed_test(fileName, name, func)

/* Block size should be the memory page size (4KiB). */
constexpr size_t BLOCK_SIZE = 1 << 12;

#if defined(_____LP64_____) || defined(_WIN64)
#define popcntBuildin _mm_popcnt_u64
#define popcntFallback popcntFallback64
using word = uint64_t;

#ifdef _DEBUG
#define MODE_STR	"64-Bit|DEBUG"
#else
#define MODE_STR	"64-Bit|RELEASE"
#endif

/* Used only by page read fallback on 64-bit mode. */
size_t popcntFallback64(uint64_t x)
{
	x = x - ((x >> 1) & 0x5555555555555555ULL);
	x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
	return (((x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL) * 0x0101010101010101ULL) >> 56;
}
#else
#define popcntBuildin _mm_popcnt_u32
#define popcntFallback popcntFallback32
using word = uint32_t;

#ifdef _DEBUG
#define MODE_STR	"32-Bit|DEBUG"
#else
#define MODE_STR	"32-Bit|RELEASE"
#endif
#endif

/* Used by SSE and AVX fallbacks. */
size_t popcntFallback32(uint32_t x)
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
size_t lineCount_ifstream(const char *fileName)
{
	std::ifstream hFile{ fileName };
	return static_cast<size_t>(std::count(std::istreambuf_iterator<char>(hFile), std::istreambuf_iterator<char>(), '\n'));
}

/*
This function represents the default C way of solving the issue.
It is pretty short and relatively easy to read, the user is required to close the file handle themselves.
This is the slowest on release mode and the second slowest on debug mode.
*/
size_t lineCount_getc(const char *fileName)
{
	FILE *hfile = fopen(fileName, "rb");

	size_t result = 0;
	for (int c = getc(hfile); c != EOF; c = getc(hfile)) result += c == '\n';

	fclose(hfile);
	return result;
}

/*
The idea of this function is to be the fastest portable function to solve the issue.
It reads from the file one memory page at a time for optimal buffering.
Instead of checking one character at a time, it instead packs them together into a word (32-bit or 64-bit).
These can be checked in parallel by some binary code, allowing for 4-8 bytes checked at one time (depending on the architecture).
*/
size_t lineCount_block_read(const char *fileName)
{
	constexpr size_t BLOCK_SIZE_WORD = BLOCK_SIZE / sizeof(word);

	FILE *hFile = fopen(fileName, "rb");

	word memory[BLOCK_SIZE_WORD];
	char *buffer = reinterpret_cast<char*>(memory);

	size_t result = 0;
	size_t bytesRead;
	while ((bytesRead = fread(buffer, sizeof(char), BLOCK_SIZE, hFile)) > 0)
	{
		memset(buffer + bytesRead, 0, BLOCK_SIZE - bytesRead);
		for (size_t i = 0; i < BLOCK_SIZE_WORD; i++)
		{
			constexpr word MASK_NL = static_cast<word>(0x0a0a0a0a0a0a0a0aLL);
			constexpr word MASK_SUB = static_cast<word>(0x0101010101010101LL);
			constexpr word MASK_AND = static_cast<word>(0x8080808080808080LL);

			const word data = memory[i] ^ MASK_NL;
			result += popcntFallback((data - MASK_SUB) & ~data & MASK_AND);
		}
	}

	fclose(hFile);
	return result;
}

/*
The SWAR family of functions in this file all do the same thing as the block read function.
They are however optimized for CPUs that support SIMD.

This function relies SS2 and POPCNT to be supported by the CPU.
This can check 16 character at one time and uses as faster popcnt.
*/
size_t lineCount_swar_sse(const char *fileName)
{
	constexpr size_t BLOCK_SIZE_AVX = BLOCK_SIZE / sizeof(__m128i);

	const __m128i MASK_NL = _mm_set1_epi8('\n');
	const __m128i MASK_SUB = _mm_set1_epi8(0x01);
	const __m128i MASK_AND = _mm_set1_epi8(0x80);

	FILE *hFile = fopen(fileName, "rb");

	__m128i memory[BLOCK_SIZE_AVX];
	char *buffer = reinterpret_cast<char*>(memory);

	size_t result = 0;
	size_t bytesRead;
	while ((bytesRead = fread(buffer, sizeof(char), BLOCK_SIZE, hFile)) > 0)
	{
		memset(buffer + bytesRead, 0, BLOCK_SIZE - bytesRead);
		for (size_t i = 0; i < BLOCK_SIZE_AVX; i++)
		{
			__m128i data = _mm_xor_si128(memory[i], MASK_NL);
			data = _mm_and_si128(_mm_sub_epi8(data, MASK_SUB), _mm_andnot_si128(data, MASK_AND));
			result += _mm_popcnt_u32(_mm_movemask_epi8(data));
		}
	}

	fclose(hFile);
	return result;
}

/*
The SWAR family of functions in this file all do the same thing as the block read function.
They are however optimized for CPUs that support SIMD.

This function relies SS2 to be supported by the CPU.
This can check 16 character at one time.
*/
size_t lineCount_swar_sse_no_buildin_popcnt(const char *fileName)
{
	constexpr size_t BLOCK_SIZE_AVX = BLOCK_SIZE / sizeof(__m128i);

	const __m128i MASK_NL = _mm_set1_epi8('\n');
	const __m128i MASK_SUB = _mm_set1_epi8(0x01);
	const __m128i MASK_AND = _mm_set1_epi8(0x80);

	FILE *hFile = fopen(fileName, "rb");

	__m128i memory[BLOCK_SIZE_AVX];
	char *buffer = reinterpret_cast<char*>(memory);

	size_t result = 0;
	size_t bytesRead;
	while ((bytesRead = fread(buffer, sizeof(char), BLOCK_SIZE, hFile)) > 0)
	{
		memset(buffer + bytesRead, 0, BLOCK_SIZE - bytesRead);
		for (size_t i = 0; i < BLOCK_SIZE_AVX; i++)
		{
			__m128i data = _mm_xor_si128(memory[i], MASK_NL);
			data = _mm_and_si128(_mm_sub_epi8(data, MASK_SUB), _mm_andnot_si128(data, MASK_AND));
			result += popcntFallback32(_mm_movemask_epi8(data));
		}
	}

	fclose(hFile);
	return result;
}

/*
The SWAR family of functions in this file all do the same thing as the block read function.
They are however optimized for CPUs that support SIMD.

This function relies AVX2 and POPCNT to be supported by the CPU.
This can check 32 character at one time and uses as faster popcnt.
This function should be the fastest.
*/
size_t lineCount_swar_avx(const char *fileName)
{
	constexpr size_t BLOCK_SIZE_AVX = BLOCK_SIZE / sizeof(__m256i);

	const __m256i MASK_NL = _mm256_set1_epi8('\n');
	const __m256i MASK_SUB = _mm256_set1_epi8(0x01);
	const __m256i MASK_AND = _mm256_set1_epi8(0x80);

	FILE *hFile = fopen(fileName, "rb");

	__m256i memory[BLOCK_SIZE_AVX];
	char *buffer = reinterpret_cast<char*>(memory);

	size_t result = 0;
	size_t bytesRead;
	while ((bytesRead = fread(buffer, sizeof(char), BLOCK_SIZE, hFile)) > 0)
	{
		memset(buffer + bytesRead, 0, BLOCK_SIZE - bytesRead);
		for (size_t i = 0; i < BLOCK_SIZE_AVX; i++)
		{
			__m256i data = _mm256_xor_si256(memory[i], MASK_NL);
			data = _mm256_and_si256(_mm256_sub_epi8(data, MASK_SUB), _mm256_andnot_si256(data, MASK_AND));
			result += _mm_popcnt_u32(_mm256_movemask_epi8(data));
		}
	}

	fclose(hFile);
	return result;
}

/*
The SWAR family of functions in this file all do the same thing as the block read function.
They are however optimized for CPUs that support SIMD.

This function relies AVX2 to be supported by the CPU.
This can check 32 character at one time.
*/
size_t lineCount_swar_avx_no_buildin_popcnt(const char *fileName)
{
	constexpr size_t BLOCK_SIZE_AVX = BLOCK_SIZE / sizeof(__m256i);

	const __m256i MASK_NL = _mm256_set1_epi8('\n');
	const __m256i MASK_SUB = _mm256_set1_epi8(0x01);
	const __m256i MASK_AND = _mm256_set1_epi8(0x80);

	FILE *hFile = fopen(fileName, "rb");

	__m256i memory[BLOCK_SIZE_AVX];
	char *buffer = reinterpret_cast<char*>(memory);

	size_t result = 0;
	size_t bytesRead;
	while ((bytesRead = fread(buffer, sizeof(char), BLOCK_SIZE, hFile)) > 0)
	{
		memset(buffer + bytesRead, 0, BLOCK_SIZE - bytesRead);
		for (size_t i = 0; i < BLOCK_SIZE_AVX; i++)
		{
			__m256i data = _mm256_xor_si256(memory[i], MASK_NL);
			data = _mm256_and_si256(_mm256_sub_epi8(data, MASK_SUB), _mm256_andnot_si256(data, MASK_AND));
			result += popcntFallback32(_mm256_movemask_epi8(data));
		}
	}

	fclose(hFile);
	return result;
}

void run_timed_test(const char *fileName, const char *functionName, size_t (*function)(const char*))
{
	const std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();
	const size_t lineCnt = function(fileName);
	const std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now();
	printf("%-30s took %03lldms to count %zu lines.\n", functionName, std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), lineCnt);
}

int main(int argc, char **argv)
{
	if (argc != 2) return EXIT_FAILURE;
	const char *fileName = argv[1];

	printf("Running performance tests on " MODE_STR " mode.\n");

	/* Check for CPU support. */
	int registers[4];
	__cpuid(registers, 1);
	const bool popcntSupported = registers[2] & 1 << 22;
	const bool sse2Supported = registers[3] & 1 << 25;
	__cpuidex(registers, 7, 0);
	const bool avx2Supported = registers[1] & 1 << 4;

	if (popcntSupported)
	{
		if (sse2Supported) RUN_TIMED_TEST(lineCount_swar_sse, "SWAR SSE2");
		if (avx2Supported) RUN_TIMED_TEST(lineCount_swar_avx, "SWAR AVX2");
	}
	else puts("CPU doesn't support POPCNT.");

	/* Check for SSE2 support. */
	if (sse2Supported) RUN_TIMED_TEST(lineCount_swar_sse_no_buildin_popcnt, "SWAR SSE2 (no CPU POPCNT)");
	else puts("CPU doesn't support SSE2.");

	/* Check for AVX2 support (extended features). */
	if (avx2Supported) RUN_TIMED_TEST(lineCount_swar_avx_no_buildin_popcnt, "SWAR AVX2 (no CPU POPCNT)");
	else puts("CPU doesn't support AVX2.");

	/* Run tests that don't depend on extended instruction sets. */
	RUN_TIMED_TEST(lineCount_block_read, "block read");
	RUN_TIMED_TEST(lineCount_getc, "getc loop");
	RUN_TIMED_TEST(lineCount_ifstream, "std ifstream count");

	puts("\nFinishes running performance tests, press any key to continue...");
	_getch();
	return EXIT_SUCCESS;
}