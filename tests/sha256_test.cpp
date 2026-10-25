#include "core/Sha256.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>

int main() {
    using mary::Sha256;
    assert(Sha256::of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(Sha256::of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(Sha256::of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Потоковый ввод кусками произвольного размера должен совпадать с одним вызовом.
    std::string big(1000003, 'a');
    Sha256 h;
    for (size_t i = 0; i < big.size(); i += 4097)
        h.update(big.data() + i, std::min<size_t>(4097, big.size() - i));
    const std::string streamed = h.hex();
    assert(streamed == Sha256::of(big));
    std::cout << streamed << "\n";
    std::cout << "sha256 OK\n";
    return 0;
}
