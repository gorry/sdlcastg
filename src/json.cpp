// sdlcastg - 小さな JSON

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace sdlcastg {

namespace {

const Json &NullJson() {
	static const Json kNull;
	return kNull;
}

void AppendUtf8(std::string *out, unsigned cp) {
	if (cp < 0x80) {
		out->push_back((char)cp);
	} else if (cp < 0x800) {
		out->push_back((char)(0xc0 | (cp >> 6)));
		out->push_back((char)(0x80 | (cp & 0x3f)));
	} else if (cp < 0x10000) {
		out->push_back((char)(0xe0 | (cp >> 12)));
		out->push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
		out->push_back((char)(0x80 | (cp & 0x3f)));
	} else {
		out->push_back((char)(0xf0 | (cp >> 18)));
		out->push_back((char)(0x80 | ((cp >> 12) & 0x3f)));
		out->push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
		out->push_back((char)(0x80 | (cp & 0x3f)));
	}
}

}  // namespace

// 再帰下降の読み取り。深さは受信側の状態程度なので制限は緩くてよいが、
// 壊れた入力で積み上がらないように上限を置く。
class JsonParser {
public:
	explicit JsonParser(const std::string &s) : s_(s), i_(0) {}

	bool ParseDocument(Json *out) {
		SkipSpace();
		if (!ParseValue(out, 0)) return false;
		SkipSpace();
		return i_ == s_.size();
	}

private:
	static const int kMaxDepth = 64;

	const std::string &s_;
	size_t i_;

	void SkipSpace() {
		while (i_ < s_.size() &&
		       (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) {
			i_++;
		}
	}

	bool Literal(const char *word) {
		const size_t n = strlen(word);
		if (s_.compare(i_, n, word) != 0) return false;
		i_ += n;
		return true;
	}

	bool ParseValue(Json *out, int depth) {
		if (depth > kMaxDepth || i_ >= s_.size()) return false;
		const char c = s_[i_];
		if (c == '{') return ParseObject(out, depth);
		if (c == '[') return ParseArray(out, depth);
		if (c == '"') {
			out->type_ = Json::kString;
			return ParseString(&out->str_);
		}
		if (c == 't') {
			if (!Literal("true")) return false;
			out->type_ = Json::kBool;
			out->bool_ = true;
			return true;
		}
		if (c == 'f') {
			if (!Literal("false")) return false;
			out->type_ = Json::kBool;
			out->bool_ = false;
			return true;
		}
		if (c == 'n') {
			if (!Literal("null")) return false;
			out->type_ = Json::kNull;
			return true;
		}
		return ParseNumber(out);
	}

	bool ParseNumber(Json *out) {
		const char *begin = s_.c_str() + i_;
		char *end = 0;
		const double v = strtod(begin, &end);
		if (end == begin) return false;
		i_ += (size_t)(end - begin);
		out->type_ = Json::kNumber;
		out->num_ = v;
		return true;
	}

	bool Hex4(unsigned *out) {
		if (i_ + 4 > s_.size()) return false;
		unsigned v = 0;
		for (int k = 0; k < 4; k++) {
			const char c = s_[i_ + (size_t)k];
			v <<= 4;
			if (c >= '0' && c <= '9') {
				v |= (unsigned)(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				v |= (unsigned)(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				v |= (unsigned)(c - 'A' + 10);
			} else {
				return false;
			}
		}
		i_ += 4;
		*out = v;
		return true;
	}

	bool ParseString(std::string *out) {
		i_++;  // '"'
		out->clear();
		while (i_ < s_.size()) {
			const char c = s_[i_++];
			if (c == '"') return true;
			if (c != '\\') {
				out->push_back(c);
				continue;
			}
			if (i_ >= s_.size()) return false;
			const char e = s_[i_++];
			switch (e) {
				case '"': out->push_back('"'); break;
				case '\\': out->push_back('\\'); break;
				case '/': out->push_back('/'); break;
				case 'b': out->push_back('\b'); break;
				case 'f': out->push_back('\f'); break;
				case 'n': out->push_back('\n'); break;
				case 'r': out->push_back('\r'); break;
				case 't': out->push_back('\t'); break;
				case 'u': {
					unsigned cp = 0;
					if (!Hex4(&cp)) return false;
					// サロゲートの対は 1 つの文字にまとめる。
					if (cp >= 0xd800 && cp < 0xdc00 && i_ + 6 <= s_.size() && s_[i_] == '\\' &&
					    s_[i_ + 1] == 'u') {
						i_ += 2;
						unsigned lo = 0;
						if (!Hex4(&lo)) return false;
						if (lo >= 0xdc00 && lo < 0xe000) {
							cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
						}
					}
					AppendUtf8(out, cp);
					break;
				}
				default:
					return false;
			}
		}
		return false;
	}

	bool ParseArray(Json *out, int depth) {
		i_++;  // '['
		out->type_ = Json::kArray;
		SkipSpace();
		if (i_ < s_.size() && s_[i_] == ']') {
			i_++;
			return true;
		}
		for (;;) {
			SkipSpace();
			out->arr_.push_back(Json());
			if (!ParseValue(&out->arr_.back(), depth + 1)) return false;
			SkipSpace();
			if (i_ >= s_.size()) return false;
			if (s_[i_] == ',') {
				i_++;
				continue;
			}
			if (s_[i_] == ']') {
				i_++;
				return true;
			}
			return false;
		}
	}

	bool ParseObject(Json *out, int depth) {
		i_++;  // '{'
		out->type_ = Json::kObject;
		SkipSpace();
		if (i_ < s_.size() && s_[i_] == '}') {
			i_++;
			return true;
		}
		for (;;) {
			SkipSpace();
			if (i_ >= s_.size() || s_[i_] != '"') return false;
			std::string key;
			if (!ParseString(&key)) return false;
			SkipSpace();
			if (i_ >= s_.size() || s_[i_] != ':') return false;
			i_++;
			SkipSpace();
			out->obj_.push_back(std::make_pair(key, Json()));
			if (!ParseValue(&out->obj_.back().second, depth + 1)) return false;
			SkipSpace();
			if (i_ >= s_.size()) return false;
			if (s_[i_] == ',') {
				i_++;
				continue;
			}
			if (s_[i_] == '}') {
				i_++;
				return true;
			}
			return false;
		}
	}
};

bool Json::Parse(const std::string &text, Json *out) {
	Json tmp;
	JsonParser p(text);
	if (!p.ParseDocument(&tmp)) {
		*out = Json();
		return false;
	}
	*out = tmp;
	return true;
}

std::string Json::Quote(const std::string &s) {
	std::string out;
	out.reserve(s.size() + 2);
	out.push_back('"');
	for (size_t i = 0; i < s.size(); i++) {
		const unsigned char c = (unsigned char)s[i];
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20) {
					char buf[8];
					snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				} else {
					// UTF-8 はそのまま通す（JSON は UTF-8 を許す）。
					out.push_back((char)c);
				}
				break;
		}
	}
	out.push_back('"');
	return out;
}

const Json &Json::operator[](const char *key) const {
	if (type_ != kObject) return NullJson();
	for (size_t i = 0; i < obj_.size(); i++) {
		if (obj_[i].first == key) return obj_[i].second;
	}
	return NullJson();
}

const Json &Json::operator[](size_t index) const {
	if (type_ != kArray || index >= arr_.size()) return NullJson();
	return arr_[index];
}

size_t Json::size() const {
	if (type_ == kArray) return arr_.size();
	if (type_ == kObject) return obj_.size();
	return 0;
}

bool Json::has(const char *key) const {
	if (type_ != kObject) return false;
	for (size_t i = 0; i < obj_.size(); i++) {
		if (obj_[i].first == key) return true;
	}
	return false;
}

std::string Json::str(const std::string &def) const {
	return (type_ == kString) ? str_ : def;
}

double Json::num(double def) const {
	return (type_ == kNumber) ? num_ : def;
}

bool Json::boolean(bool def) const {
	return (type_ == kBool) ? bool_ : def;
}

}  // namespace sdlcastg
