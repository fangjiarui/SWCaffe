#include <stdio.h>
#include <assert.h>
#include "athread.h"
#include <math.h>
//typedef double Type;
//#include "util.h"
#include "caffe/swlayers/sw_conv_layer_impl.h"
#include "caffe/util/matrix_trans.h"
//

//#ifndef ACC_TRANS
//#define ACC_TRANS
//#endif

extern SLAVE_FUN(conv_valid)();
extern SLAVE_FUN(conv_full)();
extern SLAVE_FUN(conv_pad)();
extern SLAVE_FUN(conv_full_pad)();

// high -> low
// B, N, R, C
inline int image_caffe_offset(int n, int c, int h, int w, int N, int C, int H, int W) {
  return (((n*C + c)*H + h)*W + w);
}
// R, C, N, B
inline int image_swdnn_offset(int n, int c, int h, int w, int N, int C, int H, int W) {
  return (((h*W + w)*C + c)*N + n);
}
// R, C, B, N
inline int image_swdnn_offset_back(int n, int c, int h, int w, int N, int C, int H, int W) {
  return (((h*W + w)*N + n)*C + c);
}
// No, Ni, Kr, Kc
inline int weight_caffe_offset(int no, int ni, int kr, int kc, int No, int Ni, int K) {
  return (( no*Ni + ni )*K + kr)*K + kc;
}
// Kr, Kc, No, Ni
inline int weight_swdnn_offset(int no, int ni, int kr, int kc, int No, int Ni, int K) {
  return ((( kr*K + kc )*No + no) * Ni + ni );
}
// Kr, Kc, Ni, No
inline int weight_swdnn_offset_back(int no, int ni, int kr, int kc, int No, int Ni, int K) {
  return ((( kr*K + kc )*Ni + ni) * No + no );
}

#define weight_swdnn_to_caffe(in,out,B,N,H,W) swapBN_HW(in,out,H,W,B,N)
#define weight_caffe_to_swdnn(in,out,B,N,H,W) swapBN_HW(in,out,B,N,H,W)
#define image_caffe_to_swdnn_back(in,out,B,N,H,W)  swapBN_HW(in,out,B,N,H,W)



//typedef double Type;

typedef struct ConvData_st{
  Type* input; //0
  Type* weight; //8
  Type* output; //16
  //   24,  28,  32,  36, 40,  44,  48, 52, 56 
  int _Ni, _Ri, _Ci, _No, _K, _Ro, _Co, _B, _Costride, _bCo, _pad;
}ConvData;


static int init_flag = 0; 

void sw_conv_forward_pad_impl_d(
        const Type* in, 
        const Type* weight, 
        Type* out,
        //Type* bias,
        int Ci,
        int Ri,
        int K,
        int Ni,
        int No,
        int B,
        int pad)
{
    int i;
    int cKr, cKc, cNo;
    int cRo, cCo, cB;
    int cRi, cCi, cNi;
    int Ro = Ri+2*pad-K+1 , Co = Ci+2*pad-K+1;
    Type* my_in   = (Type*)malloc(sizeof(Type)*Ri*Ci*Ni*B);
    Type* my_out  = (Type*)malloc(sizeof(Type)*Ro*Co*No*B);
    Type* my_weight = (Type*)malloc(sizeof(Type)*K*K*No*Ni);
    //Type* my_weight_ref = (Type*)malloc(sizeof(Type)*K*K*No*Ni);

    if(init_flag == 0){
      int rtcode = athread_init();
      if( rtcode != 1)
	      printf("thread init error, return code %d\n", rtcode);
      init_flag = 1 ;
    }
#ifdef MPE_TRANS
    for(cRi = 0; cRi < Ri; ++cRi)
      for(cCi = 0; cCi < Ci; ++cCi)
        for(cNi = 0; cNi < Ni; ++cNi)
          for(cB = 0; cB < B; ++cB)
            my_in[image_swdnn_offset(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)] = 
              in[image_caffe_offset(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)];
#else
    image_caffe_to_swdnn(in,my_in,B,Ni,Ri,Ci);
#endif


#ifdef MPE_TRANS
    for(cNi = 0; cNi < Ni; ++cNi)
      for(cNo = 0; cNo < No; ++cNo)
        for(cKr = 0; cKr < K; ++cKr)
          for(cKc = 0; cKc < K; ++cKc)
              my_weight[weight_swdnn_offset(cNo, cNi, cKr, cKc, No, Ni, K)] = 
                weight[weight_caffe_offset(cNo, cNi, cKr, cKc, No, Ni, K)];
#else
    weight_caffe_to_swdnn(weight,my_weight,No,Ni,K,K);
#endif

    ConvData* param = (ConvData*)malloc(sizeof(ConvData));
    param->input =  my_in;
    param->weight = my_weight;
    param->output = my_out;
	  param->_Ni = Ni;
	  param->_Ri = Ri;
	  param->_Ci = Ci;
	  param->_No = No;
	  param->_K  = K;
	  param->_Ro = Ri+2*pad-K+1;
	  param->_Co = Ci+2*pad-K+1;
	  param->_B  = B;
    param->_pad = pad;

    assert(param->_B >= 128 && param->_B%128 == 0);
    assert(param->_Ni >= 64 && param->_Ni%32 == 0);
    assert(param->_No >= 64 && param->_No%32 == 0);

    //fjr1buff 7.13
	  int Costride = (64*60*1024/8 - Ni*B-Ni*No)/(No*B);
	  param->_Costride = Costride;
    assert(Costride > 0);
	  int ldm_consume = 8*(Ni*No + No*B*Costride + Ni*B);
	  //printf("ldm comsumption is %d\n", ldm_consume/64);
	  assert(ldm_consume < 64*1024*64);
    //memset(param->output, (Type)0, sizeof(Type)*Ni*B*Ci*Ri);
	  //printf("befor init forward OK\n");

	  athread_spawn(conv_pad, param);
	  athread_join();

#ifdef MPE_TRANS 
    for(cRo = 0; cRo < Ro; ++cRo)
      for(cCo = 0; cCo < Co; ++cCo)
        for(cNo = 0; cNo < No; ++cNo)
          for(cB = 0; cB < B; ++cB)
            out[image_caffe_offset(cB, cNo, cRo, cCo, B, No, Ro, Co)] =
              my_out[image_swdnn_offset(cB, cNo, cRo, cCo, B, No, Ro, Co)];
#else
    image_swdnn_to_caffe(my_out,out,B,No,Ro,Co);
#endif
/*
    Type sum1 = 0, sum2 = 0;
    for( i = 0; i < Ni*No*K*K; ++i ) {
      if( fabs(my_weight_ref[i] - my_weight[i]) > 1e-4) {
       printf("%lf vs %lf\n", my_weight_ref[i], my_weight[i]);
      }
      sum1 += my_weight_ref[i]; sum2 += my_weight[i];
    }
    printf("check over! sum1 %lf and sum2 %lf\n", sum1, sum2);
    exit(0);
    */

    free(my_in);
    free(my_weight);
    free(my_out);
    free(param);
	  //printf("forward pad OK\n");
}

void sw_conv_forward_impl_d(
        const Type* in, 
        const Type* weight, 
        Type* out,
        //Type* bias,
        int Ci,
        int Ri,
        int K,
        int Ni,
        int No,
        int B)
{
    int i;
    int cKr, cKc, cNo;
    int cRo, cCo, cB;
    int cRi, cCi, cNi;
    int Ro = Ri-K+1 , Co = Ci-K+1;
    Type* my_in   = (Type*)malloc(sizeof(Type)*Ri*Ci*Ni*B);
    Type* my_out  = (Type*)malloc(sizeof(Type)*Ro*Co*No*B);
    Type* my_weight = (Type*)malloc(sizeof(Type)*K*K*No*Ni);
    //Type* my_weight_ref = (Type*)malloc(sizeof(Type)*K*K*No*Ni);

    if(init_flag == 0){
      int rtcode = athread_init();
      if( rtcode != 1)
	      printf("thread init error, return code %d\n", rtcode);
      init_flag = 1 ;
    }
#ifdef MPE_TRANS 
    for(cRi = 0; cRi < Ri; ++cRi)
      for(cCi = 0; cCi < Ci; ++cCi)
        for(cNi = 0; cNi < Ni; ++cNi)
          for(cB = 0; cB < B; ++cB)
            my_in[image_swdnn_offset(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)] = 
              in[image_caffe_offset(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)];
#else
    image_caffe_to_swdnn(in,my_in,B,Ni,Ri,Ci);
#endif


#ifdef MPE_TRANS 
    for(cNi = 0; cNi < Ni; ++cNi)
      for(cNo = 0; cNo < No; ++cNo)
        for(cKr = 0; cKr < K; ++cKr)
          for(cKc = 0; cKc < K; ++cKc)
              my_weight[weight_swdnn_offset(cNo, cNi, cKr, cKc, No, Ni, K)] = 
                weight[weight_caffe_offset(cNo, cNi, cKr, cKc, No, Ni, K)];
#else
    weight_caffe_to_swdnn(weight,my_weight,No,Ni,K,K);
#endif

    ConvData* param = (ConvData*)malloc(sizeof(ConvData));
    param->input =  my_in;
    param->weight = my_weight;
    param->output = my_out;
	  param->_Ni = Ni;
	  param->_Ri = Ri;
	  param->_Ci = Ci;
	  param->_No = No;
	  param->_K  = K;
	  param->_Ro = Ri-K+1;
	  param->_Co = Ci-K+1;
	  param->_B  = B;

    assert(param->_B >= 128 && param->_B%128 == 0);
    assert(param->_Ni >= 64 && param->_Ni%32 == 0);
    assert(param->_No >= 64 && param->_No%32 == 0);

	  int Costride = (64*60*1024/8 - Ni*B*2-Ni*No*2)/(No*B);
	  param->_Costride = Costride;
    assert(Costride > 0);
	  int ldm_consume = 8*(Ni*No*2 + No*B*(Costride) + Ni*B*2);
	  //printf("ldm comsumption is %d\n", ldm_consume/64);
	  assert(ldm_consume < 64*1024*64);
    //memset(param->output, (Type)0, sizeof(Type)*Ni*B*Ci*Ri);
	  //printf("befor init forward OK\n");

	  athread_spawn(conv_valid, param);
	  athread_join();

#ifdef MPE_TRANS 
    for(cRo = 0; cRo < Ro; ++cRo)
      for(cCo = 0; cCo < Co; ++cCo)
        for(cNo = 0; cNo < No; ++cNo)
          for(cB = 0; cB < B; ++cB)
            out[image_caffe_offset(cB, cNo, cRo, cCo, B, No, Ro, Co)] =
              my_out[image_swdnn_offset(cB, cNo, cRo, cCo, B, No, Ro, Co)];
#else
    image_swdnn_to_caffe(my_out,out,B,No,Ro,Co);
#endif
/*
    Type sum1 = 0, sum2 = 0;
    for( i = 0; i < Ni*No*K*K; ++i ) {
      if( fabs(my_weight_ref[i] - my_weight[i]) > 1e-4) {
       printf("%lf vs %lf\n", my_weight_ref[i], my_weight[i]);
      }
      sum1 += my_weight_ref[i]; sum2 += my_weight[i];
    }
    printf("check over! sum1 %lf and sum2 %lf\n", sum1, sum2);
    exit(0);
    */

    free(my_in);
    free(my_weight);
    free(my_out);
    free(param);
	  //printf("forward OK\n");
}

void sw_conv_backward_impl_d(
        const Type* in, 
        const Type* out_grad,
        const Type* weight,
        Type* in_grad,
        Type* weight_diff,
        //Type* bias_grad,
        int Ci,
        int Ri,
        int K,
        int Ni,
        int No,
        int B)
{

    int cKr, cKc, cNo;
    int cRo, cCo, cB;
    int cRi, cCi, cNi;
    int Ro = Ri-K+1 , Co = Ci-K+1;

    //weight_diff
    ConvData* param = (ConvData*)malloc(sizeof(ConvData));
    Type* my_in_grad = (Type*)malloc(sizeof(Type)*Ri*Ci*Ni*B);
    Type* my_in = (Type*)malloc(sizeof(Type)*Ri*Ci*Ni*B);
    Type* my_out_grad = (Type*)malloc(sizeof(Type)*Ro*Co*No*B);
    Type* my_weight_diff = (Type*)malloc(sizeof(Type)*Ni*No*K*K);

    //Transformation and rot180: in (B, N, R, C) -> (R, C, N, B)
    //TODO: Can be acc with CPEs

    if( init_flag == 0 ){
      int rtcode = athread_init();
      if( rtcode != 1 )
        printf("init error");
      init_flag = 1;
    }
#ifdef MPE_TRANS
    for(cRi = 0; cRi < Ri; ++cRi)
        for(cCi = 0; cCi < Ci; ++cCi)
            for(cNi = 0; cNi < Ni; ++cNi)
                for(cB = 0; cB < B; ++cB)
                  //my_in_grad[image_swdnn_offset_back(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)] = 
                  my_in[image_swdnn_offset_back(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)] = 
                    in[image_caffe_offset(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)];
#else
	  image_caffe_to_swdnn_back(in,my_in,B, Ni, Ri, Ci);
#endif


#ifdef MPE_TRANS
    for(cRo = 0; cRo < Ro; ++cRo)
        for(cCo = 0; cCo < Co; ++cCo)
            for(cNo = 0; cNo < No; ++cNo)
                for(cB = 0; cB < B; ++cB)
                  //my_out_grad[image_swdnn_offset_back(cB, cNo, cRo, cCo, B, No, Ro, Co)] = 
                  my_out_grad[image_swdnn_offset(cB, cNo, cRo, cCo, B, No, Ro, Co)] = 
                    out_grad[image_caffe_offset(cB, cNo, cRo, cCo, B, No, Ro, Co)];
#else
	image_caffe_to_swdnn(out_grad,my_out_grad,B, No, Ro, Co);
#endif

    //memset(my_weight_diff, 0, sizeof(Type)*Ni*No*K*K);

    param->input  = my_in;
    param->weight = my_out_grad;
    param->output = my_weight_diff;
	  param->_Ni = B;
	  param->_Ri = Ri;
	  param->_Ci = Ci;
	  param->_No = No;
	  param->_K  = Ci-K+1;
	  param->_Ro = K;
	  param->_Co = K;
	  param->_B  = Ni;

    assert(param->_B >= 128 && param->_B%128 == 0);
    assert(param->_Ni >= 64 && param->_Ni%32 == 0);
    assert(param->_No >= 64 && param->_No%32 == 0);

	  int Costride = (64*55*1024/8-param->_Ni*param->_B*2-
            param->_Ni*param->_No)/
        (param->_No*param->_B);
	  //printf("Costride is %d\n", Costride);
	  param->_Costride = Costride;
    assert(Costride > 0);

    // weight_diff = conv((in), out_grad, 'valid')
	  athread_spawn(conv_valid, param);
	  athread_join();


#ifdef MPE_TRANS
    for(cKr = 0; cKr < K; ++cKr)
        for(cKc = 0; cKc < K; ++cKc)
            for(cNo = 0; cNo < No; ++cNo)
                for(cNi = 0; cNi < Ni; ++cNi){
              weight_diff[weight_caffe_offset(cNo, cNi, cKr, cKc, No, Ni, K)]
              = my_weight_diff[weight_swdnn_offset(cNo, cNi, cKr, cKc, No, Ni, K)];
                }
#else
	  weight_swdnn_to_caffe(my_weight_diff, weight_diff,No, Ni, K, K);
#endif
	  //printf("Backward weight_diff OK\n");

    //in_grad TODO should be loaded to 64 CPEs
    //Transforamation and rot180 for Weight
    Type* my_weight   = (Type*)malloc(sizeof(Type)*No*Ni*K*K);
    //Type* my_out_grad = (Type*)malloc(sizeof(Type)*B*No*Co*Ro);

#ifdef MPE_TRANS
    for(cKr = 0; cKr < K; ++cKr)
        for(cKc = 0; cKc < K; ++cKc)
            for(cNo = 0; cNo < No; ++cNo)
                for(cNi = 0; cNi < Ni; ++cNi){
                  my_weight[weight_swdnn_offset_back(cNo, cNi, K-1-cKr, K-1-cKc, No, Ni, K)]
                    = weight[weight_caffe_offset(cNo, cNi, cKr, cKc, No, Ni, K)];
                }
#else
	  weight_caffe_to_swdnn_back(weight,my_weight,No, Ni, K, K);
#endif

    param->input  =   my_out_grad;
    param->weight =   my_weight;
    param->output =   my_in_grad;
	  param->_Ni = No;
	  param->_Ri = Ri-K+1;
	  param->_Ci = Ci-K+1;
	  param->_No = Ni;
	  param->_K  = K;
	  param->_Ro = Ri;
	  param->_Co = Ci;
	  param->_B  = B;

    Costride = (64*55*1024/8-param->_Ni*param->_B*2-param->_Ni*param->_No*2)/
            (param->_No*param->_B);
	  param->_Costride = Costride;
	  //printf("Costride is %d\n", Costride);
    assert(Costride > 0);

    //memset(my_in_grad, 0, sizeof(Type)*Ni*B*Ci*Ri);
// in_grad = conv(out_grad, rot180(weight), 'full')
	  athread_spawn(conv_full, param);
	  athread_join();
#ifdef MPE_TRANS
    for(cRi = 0; cRi < Ri; ++cRi)
        for(cCi = 0; cCi < Ci; ++cCi)
            for(cNi = 0; cNi < Ni; ++cNi)
                for(cB = 0; cB < B; ++cB)
                  in_grad[image_caffe_offset(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)] =
                    //my_in_grad[image_swdnn_offset_back(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)];
                    my_in_grad[image_swdnn_offset(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)];
#else
	  image_swdnn_to_caffe(my_in_grad,in_grad,B, Ni, Ri, Ci);
#endif
	  //printf("Backward in_grad calc is OK!\n");

    free(my_in_grad);
    free(my_in);
    free(my_weight);
    free(my_out_grad);
    free(my_weight_diff);
    free(param);
}


void sw_conv_backward_pad_impl_d(
        const Type* in,
        const Type* out_grad,
        const Type* weight,
        Type* in_grad,
        Type* weight_diff,
        //Type* bias_grad,
        int Ci,
        int Ri,
        int K,
        int Ni,
        int No,
        int B,
        int pad)
{
	  printf("begin Backward Pad Impl\n");

    int cKr, cKc, cNo;
    int cRo, cCo, cB;
    int cRi, cCi, cNi;
    int Ro = Ri+2*pad-K+1 , Co = Ci+2*pad-K+1;

    //weight_diff
    ConvData* param = (ConvData*)malloc(sizeof(ConvData));
    Type* my_in = (Type*)malloc(sizeof(Type)*Ri*Ci*Ni*B);
	  printf("here1 begin Backward Pad Impl\n");
    Type* my_out_grad = (Type*)malloc(sizeof(Type)*Ro*Co*No*B);
	  printf("here2 begin Backward Pad Impl\n");
    Type* my_weight_diff = (Type*)malloc(sizeof(Type)*Ni*No*K*K);
	  printf("here3 begin Backward Pad Impl\n");

    //Transformation and rot180: in (B, N, R, C) -> (R, C, N, B)
    //TODO: Can be acc with CPEs
    if( init_flag == 0 ){
      int rtcode = athread_init();
      if( rtcode != 1 )
        printf("init error");
      init_flag = 1;
    }
#ifdef MPE_TRANS
    for(cRi = 0; cRi < Ri; ++cRi)
        for(cCi = 0; cCi < Ci; ++cCi)
            for(cNi = 0; cNi < Ni; ++cNi)
                for(cB = 0; cB < B; ++cB)
                  my_in[image_swdnn_offset_back(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)] = 
                    in[image_caffe_offset(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)];
#else
	  image_caffe_to_swdnn_back(in,my_in,B, Ni, Ri, Ci);
#endif


#ifdef MPE_TRANS
    for(cRo = 0; cRo < Ro; ++cRo)
        for(cCo = 0; cCo < Co; ++cCo)
            for(cNo = 0; cNo < No; ++cNo)
                for(cB = 0; cB < B; ++cB)
                  my_out_grad[image_swdnn_offset(cB, cNo, cRo, cCo, B, No, Ro, Co)] = 
                    out_grad[image_caffe_offset(cB, cNo, cRo, cCo, B, No, Ro, Co)];
#else
	  image_caffe_to_swdnn(out_grad,my_out_grad,B, No, Ro, Co);
#endif

    //memset(my_weight_diff, 0, sizeof(Type)*Ni*No*K*K);

    param->input  = my_in;
    param->weight = my_out_grad;
    param->output = my_weight_diff;
	  param->_Ni  = B;
	  param->_Ri  = Ro;//+2*pad-K+1;
	  param->_Ci  = Co;//+2*pad-K+1;
	  param->_No  = No;
	  param->_K   = Ci+2*pad-K+1;
	  param->_Ro  = K;
	  param->_Co  = K;
	  param->_B   = Ni;
    param->_pad = pad;

    assert(param->_B >= 128 && param->_B%128 == 0);
    assert(param->_Ni >= 64 && param->_Ni%32 == 0);
    assert(param->_No >= 64 && param->_No%32 == 0);

    //fjr1buff 7.13
	  int Costride = (64*55*1024/8-param->_Ni*param->_B-
            param->_Ni*param->_No)/
        (param->_No*param->_B);
	  //int Costride = (64*60*1024/8 - param->_Ni*param->_B*2-param->_Ni*param->_No*2)/(param->_No*param->_B);
    //printf("Costride is %d\n", Costride);
    //int ldm_consume = 8*(param->_Ni*param->_No*2+param->_No*param->_B*Costride+param->_Ni*param->_B*2);
    //assert(ldm_consume < 64*1024*64);
	  param->_Costride = Costride;
    assert(Costride > 0);

    // weight_diff = conv(pad(in), out_grad, 'valid')
	  athread_spawn(conv_pad, param);
	  athread_join();

#ifdef MPE_TRANS
    for(cKr = 0; cKr < K; ++cKr)
        for(cKc = 0; cKc < K; ++cKc)
            for(cNo = 0; cNo < No; ++cNo)
                for(cNi = 0; cNi < Ni; ++cNi){
              weight_diff[weight_caffe_offset(cNo, cNi, cKr, cKc, No, Ni, K)]
              = my_weight_diff[weight_swdnn_offset(cNo, cNi, cKr, cKc, No, Ni, K)];
                }
#else
	  weight_swdnn_to_caffe(my_weight_diff, weight_diff,No, Ni, K, K);
#endif
	  printf("Backward weight_diff OK\n");

    free(my_weight_diff);
    free(my_in);

    //Transforamation and rot180 for Weight
    Type* my_weight   = (Type*)malloc(sizeof(Type)*No*Ni*K*K);
    Type* my_in_grad = (Type*)malloc(sizeof(Type)*Ri*Ci*Ni*B);

#ifdef MPE_TRANS
    for(cKr = 0; cKr < K; ++cKr)
        for(cKc = 0; cKc < K; ++cKc)
            for(cNo = 0; cNo < No; ++cNo)
                for(cNi = 0; cNi < Ni; ++cNi){
                  my_weight[weight_swdnn_offset_back(cNo, cNi, K-1-cKr, K-1-cKc, No, Ni, K)]
                    = weight[weight_caffe_offset(cNo, cNi, cKr, cKc, No, Ni, K)];
                }
#else
	  weight_caffe_to_swdnn_back(weight,my_weight,No, Ni, K, K);
#endif

    param->input  =   my_out_grad;
    param->weight =   my_weight;
    param->output =   my_in_grad;
	  param->_Ni = No;
	  param->_Ri = Ro;
	  param->_Ci = Co;
	  param->_No = Ni;
	  param->_K  = K;
	  param->_Ro = Ri;
	  param->_Co = Ci;
	  param->_B  = B;
	  param->_pad  = pad;

    //fjr1buff
    Costride = (64*55*1024/8-param->_Ni*param->_B-param->_Ni*param->_No)/
            (param->_No*param->_B);
	  param->_Costride = Costride;
	  //printf("Costride is %d\n", Costride);
    assert(Costride > 0);

    //memset(my_in_grad, 0, sizeof(Type)*Ni*B*Ci*Ri);
    // pad_inv(in_grad) = conv(out_grad, rot180(weight), 'full')
	  //  athread_spawn(conv_full_pad, param);
	  athread_spawn(conv_full_pad,param);
    athread_join();
#ifdef MPE_TRANS
    for(cRi = 0; cRi < Ri; ++cRi)
        for(cCi = 0; cCi < Ci; ++cCi)
            for(cNi = 0; cNi < Ni; ++cNi)
                for(cB = 0; cB < B; ++cB)
                  in_grad[image_caffe_offset(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)] =
                    //my_in_grad[image_swdnn_offset_back(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)];
                    my_in_grad[image_swdnn_offset(cB, cNi, cRi, cCi, B, Ni, Ri, Ci)];
#else
	  image_swdnn_to_caffe(my_in_grad,in_grad,B, Ni, Ri, Ci);
#endif
	  printf("Backward in_grad calc is OK!\n");

    free(my_in_grad);
    free(my_weight);
    free(my_out_grad);
    free(param);
}
