// #ifndef THREADS_SYNCH_H
// #define THREADS_SYNCH_H

#define F (1 << 14)
#define INT_MAX ((1 << 31) - 1)
#define INT_MIN (-(1 << 31))
/* x and y denote fixed_point numbers in 17.14 format
n is an integer */

int int_to_fp(int n); //integer를 fixed point로 전환
int fp_to_int_round(int x); //FP를 int로 반올림
int fp_to_int(int x); //FP를 int로 버림
int add_fp(int x, int y); //FP + FP
int add_mixed(int x, int n); //FP + int
int sub_fp(int x, int y); //FP - FP
int sub_mixed(int x, int n); //FP - int
int mult_fp(int x, int y); //FP * FP
int mult_mixed(int x, int n); //FP * int
int div_fp(int x, int y); //FP / FP
int div_mixed(int x, int n); //FP / int

int int_to_fp(int n){
    return n * F;
} //integer를 fixed point로 전환

int fp_to_int_round(int x){
    if (x >= 0) return (x + F/2) / F;
    else return (x - F/2) / F;
} //FP를 int로 반올림

int fp_to_int(int x){
    return x / F;
} //FP를 int로 버림

int add_fp(int x, int y){
    return x + y;
} //FP + FP

int add_mixed(int x, int n){
    return x + n*F;
} //FP + int

int sub_fp(int x, int y){
    return x - y;
} //FP - FP

int sub_mixed(int x, int n){
    return x - n*F;
} //FP - int

int mult_fp(int x, int y){
    return (((int64_t) x) * y /F);
} //FP * FP

int mult_mixed(int x, int n){
    return x * n;
} //FP * int

int div_fp(int x, int y){
    return (((int64_t)x) * F / y);
} //FP / FP

int div_mixed(int x, int n){
    return x / n;
} //FP / int
