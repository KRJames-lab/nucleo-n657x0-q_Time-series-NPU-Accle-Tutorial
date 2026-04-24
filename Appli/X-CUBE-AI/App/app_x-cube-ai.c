/**
  ******************************************************************************
  * @file    app_x-cube-ai.c
  * @brief   X-CUBE-AI entry points (minimal stub for N6 + stedgeai workflow)
  *
  * N6 는 X-CUBE-AI 미들웨어 대신 stedgeai 가 생성한 network.c 의
  * LL_ATON_RT_* 런타임 API 를 main.c 의 while(1) 에서 직접 호출합니다.
  * 본 파일은 CubeMX 가 자동 호출하는 두 심볼(MX_X_CUBE_AI_Init,
  * MX_X_CUBE_AI_Process) 의 링커 요구만 충족시키는 최소 스텁입니다.
  ******************************************************************************
  */

#include "app_x-cube-ai.h"
#include "stm32n6xx_hal.h"
#include "stm32n6xx_nucleo.h"

#include "ll_aton_runtime.h"
#include "ll_aton_rt_user_api.h"
#include "ll_aton_caches_interface.h"

void MX_X_CUBE_AI_Init(void)
{
    /* SRAM2~6 AXI 파워다운 해제
       (activations 버퍼가 npuRAM5(SRAM5) 를 사용하므로 필수) */
    RAMCFG_SRAM2_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    RAMCFG_SRAM3_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    RAMCFG_SRAM4_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    RAMCFG_SRAM5_AXI->CR &= ~RAMCFG_CR_SRAMSD;
    RAMCFG_SRAM6_AXI->CR &= ~RAMCFG_CR_SRAMSD;
}

/* 네트워크 c-name 은 "network" (stai_network.c 의 매크로 호출 참고).
   매크로가 만드는 심볼은 static 이므로 각 TU 에서 독립적으로 선언해야 합니다.
   network.c 에 정의된 LL_ATON_EC_Network_Init_network 등을 참조합니다. */
LL_ATON_DECLARE_NAMED_NN_INSTANCE_AND_INTERFACE(network);

static uint32_t buff_in_len, buff_out_len;

/* network_c_info.json → buffers[i].format.intq.scales[0] / .offsets[0] 값으로 교체.
   JSON에서 버퍼를 찾는 방법: graph의 inputs/outputs에 있는 buffer_id로 buffers 배열을 인덱싱.
   per-tensor signed int8일 때 "axis": -1, scales 길이 1, offsets 길이 1입니다. */
static const float IN_SCALE  = 197.50061f;
static const int   IN_ZP     = -127;
static const float OUT_SCALE = 0.324142456f;
static const int   OUT_ZP    = -73;

static inline int8_t quantize(float x){
    int32_t q = (int32_t)lrintf(x / IN_SCALE) + IN_ZP;
    if (q < -128) q = -128; else if (q > 127) q = 127;
    return (int8_t)q;
}

/* (2,1000) 원시 센서 윈도우 → (20,50,2) NHWC int8 버퍼 채우기 */
static void fill_input_from_window(const float voltage[1000],
                                   const float vibration[1000],
                                   int8_t *buf_in)
{
    /* TFLite 변환 노트 §3(F): 모델 입력은 NHWC (H=20, W=50, C=2) */
    for (int h = 0; h < 20; ++h) {
        for (int w = 0; w < 50; ++w) {
            int t = h * 50 + w;              /* 1D index */
            int idx = (h * 50 + w) * 2;      /* NHWC linear */
            buf_in[idx + 0] = quantize(voltage[t]);
            buf_in[idx + 1] = quantize(vibration[t]);
        }
    }
}

static void acquire_window(float voltage[1000], float vibration[1000])
{
    for (int i = 0; i < 1000; ++i) {
        voltage[i]  = 0.0f;
        vibration[i] = 0.0f;
    }
}

void MX_X_CUBE_AI_Process(void)
{
    /* USER CODE BEGIN 6 */
    LL_ATON_RT_RetValues_t rc = LL_ATON_RT_DONE;

    const LL_Buffer_InfoTypeDef *ib = NN_Interface_network.input_buffers_info();
    const LL_Buffer_InfoTypeDef *ob = NN_Interface_network.output_buffers_info();

    int8_t *buf_in  = (int8_t *)LL_Buffer_addr_start(&ib[0]);
    int8_t *buf_out = (int8_t *)LL_Buffer_addr_start(&ob[0]);
    buff_in_len  = ib[0].offset_end - ib[0].offset_start;   /* 2000 bytes */
    buff_out_len = ob[0].offset_end - ob[0].offset_start;   /* 1 byte */

    printf("Input  buf = %lu bytes\r\n", buff_in_len);
    printf("Output buf = %lu bytes\r\n", buff_out_len);

    LL_ATON_RT_RuntimeInit();

    float volt_win[1000], vib_win[1000];

    while (1) {
        /* 1) 센서 10초 윈도우 수집 (외부 함수) */
        acquire_window(volt_win, vib_win);

        /* 2) 양자화 + 버퍼 채우기 */
        fill_input_from_window(volt_win, vib_win, buf_in);

        /* 3) 캐시 동기화 — MCU 쪽 최신 데이터를 메모리에 내려쓰고 NPU 캐시 무효화.
           함수 인자는 (시작 주소 uintptr_t, 길이[바이트]). end 주소가 아닙니다.
           N6 NPU 캐시는 pure invalidate 가 없고 clean-invalidate 묶음만 제공합니다. */
        LL_ATON_Cache_MCU_Clean_Invalidate_Range((uintptr_t)buf_in, buff_in_len);
        LL_ATON_Cache_NPU_Clean_Invalidate_Range((uintptr_t)buf_in, buff_in_len);

        /* 4) 추론 */
        LL_ATON_RT_Init_Network(&NN_Instance_network);
        do {
            rc = LL_ATON_RT_RunEpochBlock(&NN_Instance_network);
            if (rc == LL_ATON_RT_WFE) LL_ATON_OSAL_WFE();
        } while (rc != LL_ATON_RT_DONE);

        /* 5) MCU가 출력 버퍼를 읽기 전에 invalidate (NPU가 최근에 쓴 결과가
           MCU 캐시에 반영되도록 함). 인자는 (시작 주소 uintptr_t, 길이). */
        LL_ATON_Cache_MCU_Invalidate_Range((uintptr_t)buf_out, buff_out_len);

        /* 6) int8 로짓 → float 환산·결정 */
        int8_t logit_q = buf_out[0];
        float  logit_f = OUT_SCALE * (float)(logit_q - OUT_ZP);
        int    isc_detected = (logit_f > 0.0f) ? 1 : 0;
        printf("logit=%.4f  ISC=%d\r\n", logit_f, isc_detected);

        LL_ATON_RT_DeInit_Network(&NN_Instance_network);
        BSP_LED_Toggle(LED_BLUE);  /* DIAG: inference cycle completed */
        HAL_Delay(200);
    }
    /* USER CODE END 6 */
}
