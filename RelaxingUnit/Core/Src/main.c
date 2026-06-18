/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
void debug_print(char *msg);

#define SPI_CS_PORT  GPIOA
#define SPI_CS_PIN   GPIO_PIN_4

// Added the missing delay function definition
void delay_us(uint16_t us) {
    volatile uint32_t count = us * 1;
    while(count--);
}

extern SPI_HandleTypeDef hspi1;

void TMC5160_Write_Register(uint8_t reg_addr, uint32_t value) {
    uint8_t txData[5];

    txData[0] = reg_addr | 0x80; // Write Operation Bit
    txData[1] = (value >> 24) & 0xFF;
    txData[2] = (value >> 16) & 0xFF;
    txData[3] = (value >> 8)  & 0xFF;
    txData[4] = value         & 0xFF;

    HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_PIN, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi1, txData, 5, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_PIN, GPIO_PIN_SET);
}


/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
uint16_t adc_buffer[2];  // adc_buffer[0] = PC0, adc_buffer[1] = PC1
char msg_buffer[100];    // String buffer for USB printing
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */


/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;

SPI_HandleTypeDef hspi1;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_SPI1_Init(void);
static void MX_ADC1_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void TMC5160_Init(void) {
    HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_PIN, GPIO_PIN_SET);
    HAL_Delay(10);

    // 1. CHOPCONF: Set to 4 microsteps for 4x baseline speed increase
    TMC5160_Write_Register(0x6C, 0x000100C3);

    // 2. IHOLD_IRUN: Increased IRUN to 16 for better torque at high speeds
    // Run Current (IRUN) = 16, Hold Current (IHOLD) = 2, Delay = 8
    uint32_t current_setting = (8 << 16) | (4 << 8) | (2 << 0);
    TMC5160_Write_Register(0x10, current_setting);

    // 3. GCONF: 0x00000000 turns OFF StealthChop and enables SpreadCycle
    TMC5160_Write_Register(0x00, 0x00000000);

    // 4. TPWMTHRS: Not used in pure SpreadCycle mode
    TMC5160_Write_Register(0x13, 0x00000000);

    // 5. PWMCONF: Can be left as default or omitted, as it only affects StealthChop
    TMC5160_Write_Register(0x70, 0xC40C001E);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_SPI1_Init();
  MX_USB_DEVICE_Init();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */
  TMC5160_Init();
    HAL_Delay(500);

    // Initialize background DMA processing loop for ADC1 and ADC2 pins
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buffer, 2);

    // Enable Driver Stage Output (PA9 Lowered to GND)
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_SET);
    HAL_Delay(10);

    // Fix Direction to one side permanently
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_SET); // DIR High

    //debug_print("System Initialized! Awaiting Trigger on PB8...\r\n");

    uint32_t last_print_time = 0;
      uint32_t runout_start_time = 0;
      uint32_t trigger_high_start_time = 0; // Track when input became high

      // Operational Motor States:
      // 0 = Stopped, 1 = Running, 2 = Run-out Countdown, 3 = Safety Cutoff Active
      uint8_t system_state = 0;
      GPIO_PinState last_trigger_state = GPIO_PIN_RESET;
      uint8_t safety_lockout = 0;

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	  uint32_t current_time = HAL_GetTick();

	        // ====================================================================
	        // 1. READ & MAP ADC CHANNELS DYNAMICALLY
	        // ====================================================================
	        // Map PC0 (0-4095) linearly to custom microsecond pulse delays (1000us to 50us)
	        uint16_t speed_delay_us = 1 + ((adc_buffer[0] * (2000 - 50)) / 4095);

	        // Map PC1 (0-4095) linearly to the run-out timeout length (0ms to 2000ms)
	        uint32_t runout_duration_ms = (adc_buffer[1] * 2000) / 4095;

	        // ====================================================================
	        // 2. MONITOR SERIAL OUTPUT MONITOR (Every 200ms without blocking)
	        // ====================================================================
	        if (current_time - last_print_time >= 200) {
	            last_print_time = current_time;
	            snprintf(msg_buffer, sizeof(msg_buffer),
	                     "ADC1(Spd): %d (%dus) | ADC2(Hold): %d (%ldms) | State: %d\r\n",
	                     adc_buffer[0], speed_delay_us, adc_buffer[1], runout_duration_ms, system_state);
	            debug_print(msg_buffer);
	        }

	        // ====================================================================
	        // 3. READ INPUT TRIGGER STATUS (PB8)
	        // ====================================================================
	        GPIO_PinState current_trigger = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_8);

	        // Edge Detection: Input Switched from LOW to HIGH
	        if (current_trigger == GPIO_PIN_SET && last_trigger_state == GPIO_PIN_RESET) {
	            trigger_high_start_time = current_time; // Mark start time for safety checks

	            if (!safety_lockout) {
	                HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_RESET); // Enable motor bridge
	                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET); // set the relax solenoid
	                system_state = 1;
	                //debug_print("--> Motor Started via Pin Trigger <--\r\n");
	            }
	        }

	        // Edge Detection: Input Switched from HIGH to LOW
	        if (current_trigger == GPIO_PIN_RESET && last_trigger_state == GPIO_PIN_SET) {
	            // Rule: Turn off buzzer instantly when input gets low
	            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_3, GPIO_PIN_RESET);
	            safety_lockout = 0; // Clear system safety status flag

	            if (system_state == 1) {
	                system_state = 2; // Transition to run-out state
	                runout_start_time = current_time; // Start the countdown clock
	               // debug_print("--> Trigger Released! Run-out phase active... <--\r\n");
	            } else if (system_state == 3) {
	                system_state = 0; // Return straight to stop from cutoff
	                HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_SET); // Disable driver bridge
	                //debug_print("--> Trigger Released! Exited Safety Overtime Lockout <--\r\n");
	            }
	        }
	        last_trigger_state = current_trigger;

	        // ====================================================================
	        // 4. CONTINUOUS MONITORING FOR HIGH SENSOR INPUT
	        // ====================================================================
	        if (current_trigger == GPIO_PIN_SET) {
	            uint32_t continuous_high_duration = current_time - trigger_high_start_time;

	            // Condition A: If input is high for > 5 seconds, turn on buzzer (PC3)
	            if (continuous_high_duration >= 6000) {
	                if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_3) == GPIO_PIN_RESET) {
	                    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_3, GPIO_PIN_SET); // Turn on Buzzer
	                    //debug_print("[WARNING] Input active > 6s! Buzzer Active.\r\n");
	                }
	            }

	            // Condition B: If input is high for > 30 seconds, force motor off
	            if (continuous_high_duration >= 30000) {
	                if (system_state == 1) {
	                    system_state = 3; // Jump to safety cutoff state
	                    safety_lockout = 1;
	                    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_SET); // Cut driver torque/power
	                   // debug_print("[CRITICAL] Input active > 30s! Motor Cutoff Protected.\r\n");
	                }
	            }
	        }

	        // ====================================================================
	        // 5. MOTOR STEP DRIVING STATE MACHINE
	        // ====================================================================
	        if (system_state == 1) {
	            // Trigger is active: Spin at the exact rate dictated by ADC1
	            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_SET);
	            delay_us(speed_delay_us);
	            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_RESET);
	            delay_us(speed_delay_us);
	        }
	        else if (system_state == 2) {
	            // Trigger dropped, run out the clock tracking duration configured via ADC2
	            if (current_time - runout_start_time < runout_duration_ms) {
	                HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_SET);
	                delay_us(speed_delay_us);
	                HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_RESET);
	                delay_us(speed_delay_us);
	            } else {
	                // Run-out completed! Shut down motor stepping and disable bridge
	                system_state = 0;
	                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_RESET);
	                HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_SET); // Motor relaxed
	               debug_print("--> Run-out finished. Motor Stopped. <--\r\n");
	            }
	        }
	        // If system_state == 0, the motor sits idle with constant holding torque lock.
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 72;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 3;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ENABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 2;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_10;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_144CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_11;
  sConfig.Rank = 2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream0_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_3, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_9, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_RESET);

  /*Configure GPIO pin : PC3 */
  GPIO_InitStruct.Pin = GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : PA2 PA3 PA4 PA9 */
  GPIO_InitStruct.Pin = GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PB8 */
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : PB9 */
  GPIO_InitStruct.Pin = GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
