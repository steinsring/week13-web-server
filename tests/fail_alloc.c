/* 메모리 부족 시 500 응답 검증용. 실제 서버 빌드에는 포함하지 않는다. */
#ifdef realloc
#undef realloc
#endif
#include <stddef.h>
void *test_realloc(void *pointer, size_t size)
{
    (void)pointer;
    (void)size;
    return NULL;
}
