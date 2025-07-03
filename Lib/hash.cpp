#include "hash.h"

// hash_64 - 64 bit Fowler/Noll/Vo-0 FNV-1a hash code
//
//	  http://www.isthe.com/chongo/tech/comp/fnv/index.html
//
//
// Please do not copyright this code.  This code is in the public domain.
//
// LANDON CURT NOLL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
// INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO
// EVENT SHALL LANDON CURT NOLL BE LIABLE FOR ANY SPECIAL, INDIRECT OR
// CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
// USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
// OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.
//
// By:
//	chongo <Landon Curt Noll> /\oo/\
//	  http://www.isthe.com/chongo/
//
// Share and Enjoy!	:-)

// CODING_CONVENTION_VIOLATION: Use `//` for documentation.
/*
 * Perform a 64-bit Fowler/Noll/Vo FNV-1a hash on a buffer.
 *
 * @param val: Value to hash.
 * @param len: Length of value.
 * @return: 64-bit hash.
 */
auto fds_hash(const void* val, size_t len) -> fds_hash_t
{
    const unsigned char* s{(const unsigned char*)val};
    const unsigned char* end{s + len};
    fds_hash_t hval{0xcbf29ce484222325ULL};
    // FNV-1a hash each octet of the buffer
    while (s < end)
    {
        hval = (hval ^ *s++) * 0x100000001b3ULL;
    }
    // return our new hash value
    return hval;
}

/* The ASCII-85 character set for encoding 64-bit integers */
static const char fds_a85[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";

// CODING_CONVENTION_VIOLATION: Use `//` for documentation.
/*
 * Pack a 64-bit integer into an ASCII-85 string.
 * This is a custom implementation, not fully compliant with Z85.
 * It is used for printing database names.
 *
 * @param l: The 64-bit value to pack.
 * @param out: The destination buffer. Must be at least 11 bytes.
 */
void fds_pack85(unsigned long long l, char* out)
{
    for (int i{}; i < 10 && (l != 0U); ++i)
    {
        *out++ = fds_a85[l % 85];
        l /= 85;
    }
    *out = '\0';
}