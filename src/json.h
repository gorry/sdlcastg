// sdlcastg - 小さな JSON（読み取りと、文字列の書き出し）
//
// Cast V2 のメッセージの中身は JSON。読みたいのは受信側の状態
// （transportId / sessionId / mediaSessionId / playerState など）だけなので、
// 汎用のライブラリは入れず、木に読むだけの最小のものを持つ。
// 書き出しは呼ぶ側で文字列を組み立て、文字列の値だけ Quote で囲む。

#ifndef SDLCASTG_JSON_H
#define SDLCASTG_JSON_H

#include <stddef.h>

#include <string>
#include <utility>
#include <vector>

namespace sdlcastg {

class Json {
public:
	enum Type { kNull, kBool, kNumber, kString, kArray, kObject };

	Json() : type_(kNull), num_(0.0), bool_(false) {}

	// text 全体を読む。読めなければ false（*out は null のまま）。
	static bool Parse(const std::string &text, Json *out);

	// 文字列の値として書くための引用（"…" と、要る文字のエスケープ）。
	static std::string Quote(const std::string &s);

	Type type() const { return type_; }
	bool isNull() const { return type_ == kNull; }
	bool isObject() const { return type_ == kObject; }
	bool isArray() const { return type_ == kArray; }
	bool isString() const { return type_ == kString; }
	bool isNumber() const { return type_ == kNumber; }

	// 無いキー・範囲外・型違いは null を返す（連ねて書けるように）。
	const Json &operator[](const char *key) const;
	const Json &operator[](size_t index) const;
	size_t size() const;  // 配列・オブジェクトの要素数
	bool has(const char *key) const;

	std::string str(const std::string &def = std::string()) const;
	double num(double def = 0.0) const;
	bool boolean(bool def = false) const;

private:
	friend class JsonParser;

	Type type_;
	double num_;
	bool bool_;
	std::string str_;
	std::vector<Json> arr_;
	std::vector<std::pair<std::string, Json> > obj_;
};

}  // namespace sdlcastg

#endif  // SDLCASTG_JSON_H
