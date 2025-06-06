/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32h5xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
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
#include "stm32h5xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "tusb.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void) {
	/* USER CODE BEGIN NonMaskableInt_IRQn 0 */

	/* USER CODE END NonMaskableInt_IRQn 0 */

	// RM0481 7.3.4 FLASH read operations, Read operation overview,
	//   Read access to OTP, RO and flash high-cycle data operates as follows:
	//   ... 4. If the application reads an OTP data or flash high-cycle data not previously written,
	//          a double ECC error is reported and only a word full of set bits is returned
	//          (see Section 7.3.9 for details). The read data (in 16 bits) is stored in FLASH_ECCDR register,
	//          so that the user can identify if the double ECC error is due to a virgin data or a real ECC error.
	// RM0481 7.9.10 Error correction code error (ECCC, ECCD)
	//   ... When the ECCD the flag is raised, an NMI is generated, it can be masked in SBS registers
	//       (HAL_SBS_FLASH_DisableECCNMI()) for data access (OTP, data area, RO data).
	//       Software must invalidate the instruction cache (only when ICACHE is enabled, HAL_ICACHE_Invalidate())
	//       in the NMI interrupt service routine when the ECCD flag is set.

	// If this NMI is caused by a double ECC error when reading data from flash ...
	if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_ECCD)) {
		// check that the double ECC error was detected when reading data from a high-cycling area ...
		// HAL uses the same definitions (ECCR) for the ECCDETR and ECCCORR registers
		if ((FLASH->ECCDETR & FLASH_ECCR_DATA_ECC) != 0u) {
			// retrieve the read data (16 bits) that caused the double ECC error
			// and determine if the double ECC error is due to a virgin data (0xFFFF) or if it is a real ECC error
			if ((FLASH->ECCDR & FLASH_ECCDR_FAIL_DATA) == 0xFFFF) {
				// if it was a false positive due to a virgin data (0xFFFF), reset the ECCD flag
				// and return (exit exception handling and resume normal execution)
				__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ECCD);
				return;
			}
		}
	}

	/* USER CODE BEGIN NonMaskableInt_IRQn 1 */
	Error_Handler();
	/* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void) {
	/* USER CODE BEGIN HardFault_IRQn 0 */
	Error_Handler();
	/* USER CODE END HardFault_IRQn 0 */
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void) {
	/* USER CODE BEGIN MemoryManagement_IRQn 0 */
	Error_Handler();
	/* USER CODE END MemoryManagement_IRQn 0 */
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void) {
	/* USER CODE BEGIN BusFault_IRQn 0 */
	Error_Handler();
	/* USER CODE END BusFault_IRQn 0 */
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void) {
	/* USER CODE BEGIN UsageFault_IRQn 0 */
	Error_Handler();
	/* USER CODE END UsageFault_IRQn 0 */
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void) {
	/* USER CODE BEGIN SVCall_IRQn 0 */

	/* USER CODE END SVCall_IRQn 0 */
	/* USER CODE BEGIN SVCall_IRQn 1 */

	/* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void) {
	/* USER CODE BEGIN DebugMonitor_IRQn 0 */

	/* USER CODE END DebugMonitor_IRQn 0 */
	/* USER CODE BEGIN DebugMonitor_IRQn 1 */

	/* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void) {
	/* USER CODE BEGIN PendSV_IRQn 0 */

	/* USER CODE END PendSV_IRQn 0 */
	/* USER CODE BEGIN PendSV_IRQn 1 */

	/* USER CODE END PendSV_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void) {
	/* USER CODE BEGIN SysTick_IRQn 0 */

	/* USER CODE END SysTick_IRQn 0 */
	HAL_IncTick();
	/* USER CODE BEGIN SysTick_IRQn 1 */

	/* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32H5xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32h5xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles EXTI Line13 interrupt.
  */
void EXTI13_IRQHandler(void) {
	/* USER CODE BEGIN EXTI13_IRQn 0 */

	/* USER CODE END EXTI13_IRQn 0 */
	BSP_PB_IRQHandler(BUTTON_USER);
	/* USER CODE BEGIN EXTI13_IRQn 1 */

	/* USER CODE END EXTI13_IRQn 1 */
}

/* USER CODE BEGIN 1 */

void USB_DRD_FS_IRQHandler(void) {
	tud_int_handler(0);
}

/* USER CODE END 1 */
