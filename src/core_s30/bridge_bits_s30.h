#ifndef CORE_S30_BRIDGE_BITS_H
#define CORE_S30_BRIDGE_BITS_H
//=============================================================================//
//
// Purpose: LSB-first bit reader/writer for S3 wire data, the engine bf_read
// layout (S30.1 == S21), and Valve LZSS (the S3 netchannel's reliable
// compression). Engine-free.
//
//=============================================================================//
#include <cstdint>
#include <cstring>

// Bit reader over a byte buffer (S3 wire order: LSB first).
struct S30BitReader_t
{
	const uint8_t* m_pData;
	int m_nTotalBits;
	int m_nPos;
	bool m_bOverflow;

	S30BitReader_t(const uint8_t* pData, int nBytes, int nStartBit = 0)
		: m_pData(pData), m_nTotalBits(nBytes * 8), m_nPos(nStartBit), m_bOverflow(false) {}

	int BitsLeft(void) const { return m_nTotalBits - m_nPos; }

	uint32_t Read(int nBits)
	{
		if (nBits <= 0)
			return 0;
		if (m_nPos + nBits > m_nTotalBits)
		{
			m_bOverflow = true;
			m_nPos = m_nTotalBits;
			return 0;
		}
		uint32_t v = 0;
		for (int b = 0; b < nBits; ++b, ++m_nPos)
		{
			if (m_pData[m_nPos >> 3] & (1u << (m_nPos & 7)))
				v |= 1u << b;
		}
		return v;
	}
	uint64_t Read64(void)
	{
		const uint64_t lo = Read(32);
		return lo | (static_cast<uint64_t>(Read(32)) << 32);
	}
	// Reads up to nMax bytes of a NUL-terminated string into psz (always
	// terminated). Returns false on overflow.
	bool ReadString(char* psz, int nOut, int nMax)
	{
		int n = 0;
		for (int i = 0; i < nMax; ++i)
		{
			const char c = static_cast<char>(Read(8));
			if (m_bOverflow)
				break;
			if (!c)
				break;
			if (n < nOut - 1)
				psz[n++] = c;
		}
		psz[n] = '\0';
		return !m_bOverflow;
	}
	void Skip(int nBits)
	{
		if (nBits < 0 || m_nPos + nBits > m_nTotalBits)
		{
			m_bOverflow = true;
			m_nPos = m_nTotalBits;
			return;
		}
		m_nPos += nBits;
	}
	void ReadBytes(uint8_t* pOut, int nBytes)
	{
		for (int i = 0; i < nBytes; ++i)
			pOut[i] = static_cast<uint8_t>(Read(8));
	}
};

// Bit writer into a caller-owned buffer.
struct S30BitWriter_t
{
	uint8_t* m_pData;
	int m_nMaxBits;
	int m_nPos;
	bool m_bOverflow;

	S30BitWriter_t(uint8_t* pData, int nBytes) : m_pData(pData), m_nMaxBits(nBytes * 8), m_nPos(0), m_bOverflow(false)
	{
		memset(pData, 0, nBytes);
	}

	void Write(uint64_t v, int nBits)
	{
		if (m_nPos + nBits > m_nMaxBits)
		{
			m_bOverflow = true;
			return;
		}
		for (int b = 0; b < nBits; ++b, ++m_nPos)
		{
			if ((v >> b) & 1)
				m_pData[m_nPos >> 3] |= static_cast<uint8_t>(1u << (m_nPos & 7));
		}
	}
	// Writes the string and its NUL, cut so the NUL fits inside nMax bytes.
	void WriteString(const char* psz, int nMax)
	{
		int i = 0;
		for (; i < nMax - 1 && psz[i]; ++i)
			Write(static_cast<uint8_t>(psz[i]), 8);
		Write(0, 8);
	}
	void WriteBytes(const uint8_t* p, int n)
	{
		for (int i = 0; i < n; ++i)
			Write(p[i], 8);
	}
	// Copies nBits from a reader (advancing it).
	void WriteFrom(S30BitReader_t& r, int nBits)
	{
		while (nBits > 0)
		{
			const int n = nBits > 32 ? 32 : nBits;
			Write(r.Read(n), n);
			nBits -= n;
		}
	}
	int Bytes(void) const { return (m_nPos + 7) / 8; }
};

// Engine bf_read (0x40 bytes; same layout in S21 and S30.1, s30/docs/12).
namespace S30BfRead
{
	const int OFF_OVERFLOW = 0x08;
	const int OFF_DATABITS = 0x10;
	const int OFF_DATABYTES = 0x18;
	const int OFF_INBUFWORD = 0x20;
	const int OFF_BITSAVAIL = 0x24;
	const int OFF_PDATAIN = 0x28;
	const int OFF_PBUFEND = 0x30;
	const int OFF_PDATA = 0x38;
	const int SIZE = 0x40;

	// Bits read so far (the formula the engine's ProcessMessages uses).
	inline int64_t BitsRead(const uint8_t* bf)
	{
		const uint8_t* const pData = *reinterpret_cast<const uint8_t* const*>(bf + OFF_PDATA);
		const uint8_t* const pIn = *reinterpret_cast<const uint8_t* const*>(bf + OFF_PDATAIN);
		const int64_t nBytes = *reinterpret_cast<const int64_t*>(bf + OFF_DATABYTES);
		const int32_t nAvail = *reinterpret_cast<const int32_t*>(bf + OFF_BITSAVAIL);
		if (!pData)
			return 0;
		return 8 * ((nBytes & 3) + 4 * ((pIn - pData) >> 2)) - nAvail;
	}
	inline bool Overflowed(const uint8_t* bf)
	{
		return bf[OFF_OVERFLOW] != 0;
	}
}

// Valve LZSS (tier1/lzss.cpp CLZSS::SafeUncompress). Returns the output size, 0 on error.
inline unsigned int S30Lzss_Uncompress(const uint8_t* pInput, unsigned int nInput, uint8_t* pOutput, unsigned int nOutMax)
{
	const unsigned int kId = ('S' << 24) | ('S' << 16) | ('Z' << 8) | 'L'; // "LZSS" little endian
	if (!pInput || !pOutput || nInput < 8)
		return 0;
	unsigned int nId, nActual;
	memcpy(&nId, pInput, 4);
	memcpy(&nActual, pInput + 4, 4);
	if (nId != kId || !nActual || nActual > nOutMax)
		return 0;
	const uint8_t* const pEnd = pInput + nInput;
	const uint8_t* p = pInput + 8;
	unsigned int nTotal = 0;
	int nCmd = 0, nGet = 0;
	for (;;)
	{
		if (!nGet)
		{
			if (p >= pEnd)
				return 0;
			nCmd = *p++;
		}
		nGet = (nGet + 1) & 7;
		if (nCmd & 1)
		{
			if (p + 2 > pEnd)
				return 0;
			int nPos = *p++ << 4;
			nPos |= *p >> 4;
			const int nCount = (*p++ & 0x0F) + 1;
			if (nCount == 1)
				break;
			if (static_cast<unsigned int>(nPos + 1) > nTotal || nTotal + nCount > nOutMax)
				return 0;
			const uint8_t* pSrc = pOutput - nPos - 1;
			for (int i = 0; i < nCount; ++i)
				*pOutput++ = *pSrc++;
			nTotal += nCount;
		}
		else
		{
			if (p >= pEnd || nTotal + 1 > nOutMax)
				return 0;
			*pOutput++ = *p++;
			++nTotal;
		}
		nCmd >>= 1;
	}
	return nTotal == nActual ? nTotal : 0;
}

#endif // CORE_S30_BRIDGE_BITS_H
