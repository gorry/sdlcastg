// sdlcastg - Cast V2 の CastMessage（protobuf）の符号化と復号

#include "castmsg.h"

namespace sdlcastg {

const char kNsConnection[] = "urn:x-cast:com.google.cast.tp.connection";
const char kNsHeartbeat[] = "urn:x-cast:com.google.cast.tp.heartbeat";
const char kNsReceiver[] = "urn:x-cast:com.google.cast.receiver";
const char kNsMedia[] = "urn:x-cast:com.google.cast.media";

namespace {

void PutVarint(std::string *out, uint64_t v) {
	while (v >= 0x80) {
		out->push_back((char)(0x80 | (v & 0x7f)));
		v >>= 7;
	}
	out->push_back((char)v);
}

void PutInt(std::string *out, int field, uint64_t v) {
	PutVarint(out, ((uint64_t)field << 3) | 0);
	PutVarint(out, v);
}

void PutString(std::string *out, int field, const std::string &s) {
	PutVarint(out, ((uint64_t)field << 3) | 2);
	PutVarint(out, s.size());
	out->append(s);
}

bool GetVarint(const uint8_t *p, size_t size, size_t *i, uint64_t *out) {
	uint64_t v = 0;
	int shift = 0;
	while (*i < size && shift < 64) {
		const uint8_t b = p[(*i)++];
		v |= (uint64_t)(b & 0x7f) << shift;
		if ((b & 0x80) == 0) {
			*out = v;
			return true;
		}
		shift += 7;
	}
	return false;
}

}  // namespace

std::string EncodeCastMessage(const CastMessage &m) {
	std::string body;
	PutInt(&body, 1, 0);  // CASTV2_1_0
	PutString(&body, 2, m.source);
	PutString(&body, 3, m.destination);
	PutString(&body, 4, m.ns);
	PutInt(&body, 5, 0);  // STRING
	PutString(&body, 6, m.payload);

	std::string out;
	const uint32_t n = (uint32_t)body.size();
	out.push_back((char)(n >> 24));
	out.push_back((char)(n >> 16));
	out.push_back((char)(n >> 8));
	out.push_back((char)n);
	out.append(body);
	return out;
}

bool DecodeCastMessage(const uint8_t *data, size_t size, CastMessage *out) {
	*out = CastMessage();
	size_t i = 0;
	while (i < size) {
		uint64_t key = 0;
		if (!GetVarint(data, size, &i, &key)) return false;
		const int field = (int)(key >> 3);
		const int wire = (int)(key & 7);
		if (wire == 0) {
			uint64_t v = 0;
			if (!GetVarint(data, size, &i, &v)) return false;
			if (field == 5) out->binary = (v == 1);
		} else if (wire == 2) {
			uint64_t n = 0;
			if (!GetVarint(data, size, &i, &n)) return false;
			if (n > size - i) return false;
			const std::string s((const char *)data + i, (size_t)n);
			i += (size_t)n;
			switch (field) {
				case 2: out->source = s; break;
				case 3: out->destination = s; break;
				case 4: out->ns = s; break;
				case 6: out->payload = s; break;
				default: break;  // 7 (payload_binary) など
			}
		} else if (wire == 5) {
			if (size - i < 4) return false;
			i += 4;
		} else if (wire == 1) {
			if (size - i < 8) return false;
			i += 8;
		} else {
			return false;
		}
	}
	return true;
}

}  // namespace sdlcastg
