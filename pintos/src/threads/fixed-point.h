#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

/* 17.14 Sabit Noktalı formatı için çarpan (2^14) */
#define F (1 << 14)

/* Tamsayıyı Sabit Noktalıya Çevirme */
#define INT_TO_FP(n) ((n) * (F))

/* Sabit Noktalıyı Tamsayıya Çevirme (Sıfıra Yuvarlama) */
#define FP_TO_INT_ZERO(x) ((x) / (F))

/* Sabit Noktalıyı Tamsayıya Çevirme (En Yakına Yuvarlama) */
#define FP_TO_INT_NEAREST(x) ((x) >= 0 ? ((x) + (F) / 2) / (F) : ((x) - (F) / 2) / (F))

/* İki Sabit Noktalı Sayıyı Toplama/Çıkarma */
#define ADD_FP(x, y) ((x) + (y))
#define SUB_FP(x, y) ((x) - (y))

/* Sabit Noktalı Sayı ile Tamsayıyı Toplama/Çıkarma */
#define ADD_FP_INT(x, n) ((x) + (n) * (F))
#define SUB_FP_INT(x, n) ((x) - (n) * (F))

/* İki Sabit Noktalı Sayıyı Çarpma/Bölme (Taşmayı önlemek için int64_t cast işlemi) */
#define MUL_FP(x, y) ((int32_t) (((int64_t) (x)) * (y) / (F)))
#define DIV_FP(x, y) ((int32_t) ((((int64_t) (x)) * (F)) / (y)))

/* Sabit Noktalı Sayı ile Tamsayıyı Çarpma/Bölme */
#define MUL_FP_INT(x, n) ((x) * (n))
#define DIV_FP_INT(x, n) ((x) / (n))

#endif /* threads/fixed-point.h */