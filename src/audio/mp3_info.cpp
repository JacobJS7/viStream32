#include "audio/mp3_info.h"

#define MP3_TEXT_MAX_BYTES 96      // maks. długość surowego tekstu ramki ID3
#define MP3_SCAN_BYTES 2048        // ile bajtów od końca ID3v2 szukamy pierwszej ramki

// Statyczny bufor: funkcję woła też audioFeedTask (mały stos), a wywołania są
// serializowane przez sdMutex.
static uint8_t scanBuffer[MP3_SCAN_BYTES];

static const uint16_t kBitrateMpeg1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const uint16_t kBitrateMpeg2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
static const uint32_t kSampleRates[3][3] = {
	{44100, 48000, 32000},  // MPEG1
	{22050, 24000, 16000},  // MPEG2
	{11025, 12000, 8000},   // MPEG2.5
};

static uint32_t readBE32(const uint8_t *p) {
	return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
		(static_cast<uint32_t>(p[2]) << 8) | p[3];
}

static uint32_t readSyncsafe32(const uint8_t *p) {
	return (static_cast<uint32_t>(p[0] & 0x7F) << 21) | (static_cast<uint32_t>(p[1] & 0x7F) << 14) |
		(static_cast<uint32_t>(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

static void appendUtf8(String &out, uint32_t codePoint) {
	if (codePoint < 0x80) {
		out += static_cast<char>(codePoint);
	} else if (codePoint < 0x800) {
		out += static_cast<char>(0xC0 | (codePoint >> 6));
		out += static_cast<char>(0x80 | (codePoint & 0x3F));
	} else if (codePoint < 0x10000) {
		out += static_cast<char>(0xE0 | (codePoint >> 12));
		out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (codePoint & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (codePoint >> 18));
		out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (codePoint & 0x3F));
	}
}

// Usuwa niedokończoną sekwencję UTF-8 na końcu (po obcięciu tekstu).
static void trimIncompleteUtf8(String &text) {
	int length = text.length();
	int start = length - 1;
	while (start >= 0 && (static_cast<uint8_t>(text[start]) & 0xC0) == 0x80) --start;
	if (start < 0) return;
	uint8_t lead = static_cast<uint8_t>(text[start]);
	int expected = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
	if (length - start < expected) text.remove(start);
}

// Dekoduje zawartość ramki tekstowej ID3 (bajt kodowania + tekst) do UTF-8.
static String decodeId3Text(const uint8_t *data, size_t length) {
	String out;
	if (length < 2) return out;
	uint8_t encoding = data[0];
	const uint8_t *text = data + 1;
	size_t textLength = length - 1;

	if (encoding == 0 || encoding == 3) {  // ISO-8859-1 / UTF-8
		for (size_t index = 0; index < textLength && text[index] != 0; ++index) {
			if (encoding == 3 || text[index] < 0x80) out += static_cast<char>(text[index]);
			else appendUtf8(out, text[index]);
		}
	} else if (encoding == 1 || encoding == 2) {  // UTF-16 z BOM / UTF-16BE
		bool bigEndian = encoding == 2;
		size_t index = 0;
		if (encoding == 1 && textLength >= 2) {
			if (text[0] == 0xFF && text[1] == 0xFE) { bigEndian = false; index = 2; }
			else if (text[0] == 0xFE && text[1] == 0xFF) { bigEndian = true; index = 2; }
		}
		for (; index + 1 < textLength; index += 2) {
			uint32_t unit = bigEndian ? (text[index] << 8) | text[index + 1] : (text[index + 1] << 8) | text[index];
			if (unit == 0) break;
			if (unit >= 0xD800 && unit <= 0xDBFF && index + 3 < textLength) {
				uint32_t low = bigEndian ? (text[index + 2] << 8) | text[index + 3] : (text[index + 3] << 8) | text[index + 2];
				if (low >= 0xDC00 && low <= 0xDFFF) {
					appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
					index += 2;
					continue;
				}
			}
			appendUtf8(out, unit);
		}
	}
	trimIncompleteUtf8(out);
	out.trim();
	return out;
}

// Czyta tytuł/wykonawcę z ID3v2 i zwraca offset końca tagu (0 gdy brak tagu).
static uint32_t parseId3v2(File &file, uint32_t fileSize, Mp3Info &out) {
	uint8_t header[10];
	file.seek(0);
	if (file.read(header, 10) != 10 || memcmp(header, "ID3", 3) != 0) return 0;

	uint8_t major = header[3];
	uint8_t flags = header[5];
	uint32_t tagSize = readSyncsafe32(header + 6);
	uint32_t tagEnd = 10 + tagSize + ((flags & 0x10) ? 10 : 0);
	if (major < 2 || major > 4 || tagEnd > fileSize) return tagEnd <= fileSize ? tagEnd : 0;

	uint32_t position = 10;
	if ((flags & 0x40) && major >= 3) {  // nagłówek rozszerzony
		uint8_t extended[4];
		file.seek(position);
		if (file.read(extended, 4) != 4) return tagEnd;
		position += major == 4 ? readSyncsafe32(extended) : readBE32(extended) + 4;
	}

	const uint8_t frameHeaderSize = major == 2 ? 6 : 10;
	uint32_t frameDataEnd = 10 + tagSize;
	while (position + frameHeaderSize <= frameDataEnd && (out.title.isEmpty() || out.artist.isEmpty())) {
		uint8_t frameHeader[10];
		file.seek(position);
		if (file.read(frameHeader, frameHeaderSize) != frameHeaderSize || frameHeader[0] == 0) break;

		uint32_t frameSize;
		bool isTitle;
		bool isArtist;
		bool unreadable = false;
		if (major == 2) {
			frameSize = (frameHeader[3] << 16) | (frameHeader[4] << 8) | frameHeader[5];
			isTitle = memcmp(frameHeader, "TT2", 3) == 0;
			isArtist = memcmp(frameHeader, "TP1", 3) == 0;
		} else {
			frameSize = major == 4 ? readSyncsafe32(frameHeader + 4) : readBE32(frameHeader + 4);
			isTitle = memcmp(frameHeader, "TIT2", 4) == 0;
			isArtist = memcmp(frameHeader, "TPE1", 4) == 0;
			// kompresja / szyfrowanie — pomijamy treść
			unreadable = major == 4 ? (frameHeader[9] & 0x0C) != 0 : (frameHeader[9] & 0xC0) != 0;
		}
		position += frameHeaderSize;
		if (frameSize == 0 || position + frameSize > frameDataEnd) break;

		if ((isTitle || isArtist) && !unreadable) {
			uint8_t text[MP3_TEXT_MAX_BYTES];
			size_t toRead = min(static_cast<size_t>(frameSize), sizeof(text));
			if (file.read(text, toRead) == toRead) {
				String value = decodeId3Text(text, toRead);
				if (isTitle) out.title = value;
				else out.artist = value;
			}
		}
		position += frameSize;  // np. okładka APIC — przeskok bez czytania
	}
	return tagEnd;
}

// ID3v1 (128 B na końcu): zwraca true gdy jest; uzupełnia brakujące pola.
static bool parseId3v1(File &file, uint32_t fileSize, Mp3Info &out) {
	if (fileSize < 128) return false;
	uint8_t tag[128];
	file.seek(fileSize - 128);
	if (file.read(tag, 128) != 128 || memcmp(tag, "TAG", 3) != 0) return false;
	auto field = [&](uint8_t offset) {
		String value;
		for (uint8_t index = 0; index < 30 && tag[offset + index] != 0; ++index) {
			uint8_t character = tag[offset + index];
			if (character < 0x80) value += static_cast<char>(character);
			else appendUtf8(value, character);
		}
		value.trim();
		return value;
	};
	if (out.title.isEmpty()) out.title = field(3);
	if (out.artist.isEmpty()) out.artist = field(33);
	return true;
}

struct FrameHeader {
	uint8_t versionIndex;   // 0 MPEG1, 1 MPEG2, 2 MPEG2.5
	bool mono;
	uint16_t bitrateKbps;
	uint32_t sampleRate;
	uint16_t samplesPerFrame;
	uint32_t frameLength;
};

static bool parseFrameHeader(const uint8_t *p, FrameHeader &header) {
	if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
	uint8_t versionBits = (p[1] >> 3) & 0x03;
	uint8_t layerBits = (p[1] >> 1) & 0x03;
	uint8_t bitrateIndex = p[2] >> 4;
	uint8_t sampleRateIndex = (p[2] >> 2) & 0x03;
	if (versionBits == 1 || layerBits != 1 || bitrateIndex == 0 || bitrateIndex == 15 || sampleRateIndex == 3) return false;

	header.versionIndex = versionBits == 3 ? 0 : (versionBits == 2 ? 1 : 2);
	header.mono = (p[3] >> 6) == 3;
	header.bitrateKbps = header.versionIndex == 0 ? kBitrateMpeg1[bitrateIndex] : kBitrateMpeg2[bitrateIndex];
	header.sampleRate = kSampleRates[header.versionIndex][sampleRateIndex];
	header.samplesPerFrame = header.versionIndex == 0 ? 1152 : 576;
	uint32_t padding = (p[2] >> 1) & 0x01;
	uint32_t coefficient = header.versionIndex == 0 ? 144 : 72;
	header.frameLength = coefficient * header.bitrateKbps * 1000 / header.sampleRate + padding;
	return header.frameLength >= 24;
}

static void applyFallback(Mp3Info &out, uint32_t fileSize, uint32_t fallbackBytesPerSec) {
	out.valid = false;
	out.audioBytes = fileSize > out.audioStart ? fileSize - out.audioStart : fileSize;
	out.bytesPerSec = fallbackBytesPerSec;
	out.durationSec = max(1UL, out.audioBytes / fallbackBytesPerSec);
	out.bitrateKbps = 0;
	out.vbr = false;
	out.hasToc = false;
}

bool readMp3Info(File &file, Mp3Info &out, uint32_t fallbackBytesPerSec) {
	out = Mp3Info();
	uint32_t fileSize = static_cast<uint32_t>(file.size());

	uint32_t id3End = parseId3v2(file, fileSize, out);
	bool hasId3v1 = parseId3v1(file, fileSize, out);
	uint32_t audioEnd = hasId3v1 ? fileSize - 128 : fileSize;
	out.audioStart = id3End;

	file.seek(id3End);
	size_t scanned = file.read(scanBuffer, sizeof(scanBuffer));

	// Pierwsza ramka: nagłówek + (jeśli mieści się w buforze) zgodny nagłówek następnej.
	FrameHeader header;
	int frameOffset = -1;
	for (size_t index = 0; index + 4 <= scanned; ++index) {
		if (!parseFrameHeader(scanBuffer + index, header)) continue;
		size_t next = index + header.frameLength;
		FrameHeader nextHeader;
		if (next + 4 <= scanned && !parseFrameHeader(scanBuffer + next, nextHeader)) continue;
		frameOffset = static_cast<int>(index);
		break;
	}

	if (frameOffset < 0) {
		applyFallback(out, audioEnd, fallbackBytesPerSec);
		file.seek(out.audioStart);
		return false;
	}

	out.valid = true;
	out.audioStart = id3End + frameOffset;
	out.audioBytes = audioEnd > out.audioStart ? audioEnd - out.audioStart : 0;
	out.bitrateKbps = header.bitrateKbps;
	out.sampleRate = header.sampleRate;

	uint32_t totalFrames = 0;
	const uint8_t *frame = scanBuffer + frameOffset;
	size_t frameAvailable = scanned - frameOffset;
	size_t xingOffset = 4 + (header.versionIndex == 0 ? (header.mono ? 17 : 32) : (header.mono ? 9 : 17));

	if (xingOffset + 8 <= frameAvailable &&
		(memcmp(frame + xingOffset, "Xing", 4) == 0 || memcmp(frame + xingOffset, "Info", 4) == 0)) {
		out.vbr = memcmp(frame + xingOffset, "Xing", 4) == 0;
		uint32_t flags = readBE32(frame + xingOffset + 4);
		size_t cursor = xingOffset + 8;
		if ((flags & 0x1) && cursor + 4 <= frameAvailable) { totalFrames = readBE32(frame + cursor); cursor += 4; }
		if ((flags & 0x2) && cursor + 4 <= frameAvailable) {
			uint32_t xingBytes = readBE32(frame + cursor);
			if (xingBytes > 0 && xingBytes <= out.audioBytes) out.audioBytes = xingBytes;
			cursor += 4;
		}
		if ((flags & 0x4) && cursor + 100 <= frameAvailable) {
			memcpy(out.toc, frame + cursor, 100);
			out.hasToc = true;
		}
	} else if (36 + 18 <= frameAvailable && memcmp(frame + 36, "VBRI", 4) == 0) {
		out.vbr = true;
		uint32_t vbriBytes = readBE32(frame + 36 + 10);
		totalFrames = readBE32(frame + 36 + 14);
		if (vbriBytes > 0 && vbriBytes <= out.audioBytes) out.audioBytes = vbriBytes;
	}

	if (totalFrames > 0) {
		uint64_t samples = static_cast<uint64_t>(totalFrames) * header.samplesPerFrame;
		out.durationSec = max(1UL, static_cast<uint32_t>(samples / header.sampleRate));
	} else {
		// CBR bez nagłówka Xing: czas z rozmiaru i bitrate.
		out.durationSec = max(1UL, static_cast<uint32_t>(static_cast<uint64_t>(out.audioBytes) * 8 / (header.bitrateKbps * 1000UL)));
	}
	out.bytesPerSec = max(1UL, out.audioBytes / out.durationSec);

	file.seek(out.audioStart);
	return true;
}

uint32_t mp3ByteOffsetForSecond(const Mp3Info &info, uint32_t sec, uint32_t fileSize) {
	uint32_t offset;
	if (sec >= info.durationSec) {
		offset = info.audioStart + info.audioBytes;
	} else if (info.hasToc) {
		// Interpolacja w tablicy TOC Xing (100 punktów procentowych czasu).
		float percent = 100.0f * sec / info.durationSec;
		int index = min(99, static_cast<int>(percent));
		float low = info.toc[index];
		float high = index < 99 ? info.toc[index + 1] : 256.0f;
		float fraction = low + (high - low) * (percent - index);
		offset = info.audioStart + static_cast<uint32_t>(fraction / 256.0f * info.audioBytes);
	} else {
		offset = info.audioStart + static_cast<uint32_t>(static_cast<uint64_t>(sec) * info.bytesPerSec);
	}
	if (fileSize > 0 && offset >= fileSize) offset = fileSize - 1;
	return offset;
}
