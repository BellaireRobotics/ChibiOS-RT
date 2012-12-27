/*
    ChibiOS/RT - Copyright (C) 2006,2007,2008,2009,2010,
                 2011,2012 Giovanni Di Sirio.

    This file is part of ChibiOS/RT.

    ChibiOS/RT is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    ChibiOS/RT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file    STM32F3xx/adc_lld.c
 * @brief   STM32F3xx ADC subsystem low level driver source.
 *
 * @addtogroup ADC
 * @{
 */

#include "ch.h"
#include "hal.h"

#if HAL_USE_ADC || defined(__DOXYGEN__)

/*===========================================================================*/
/* Driver local definitions.                                                 */
/*===========================================================================*/

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

/** @brief ADC1 driver identifier.*/
#if STM32_ADC_USE_ADC1 || defined(__DOXYGEN__)
ADCDriver ADCD1;
#endif

/** @brief ADC1 driver identifier.*/
#if STM32_ADC_USE_ADC3 || defined(__DOXYGEN__)
ADCDriver ADCD3;
#endif

/*===========================================================================*/
/* Driver local variables.                                                   */
/*===========================================================================*/

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

/**
 * @brief   Enables the ADC voltage regulator.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 */
static void adc_lld_vreg_on(ADCDriver *adcp) {

  adcp->adcm->CR = ADC_CR_ADVREGEN_0;
#if STM32_ADC_DUAL_MODE
  adcp->adcs->CR = ADC_CR_ADVREGEN_0;
#endif
  halPolledDelay(US2RTT(10));
}

/**
 * @brief   Disables the ADC voltage regulator.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 */
static void adc_lld_vreg_off(ADCDriver *adcp) {

  adcp->adcm->CR = ADC_CR_ADVREGEN_1;
#if STM32_ADC_DUAL_MODE
  adcp->adcs->CR = ADC_CR_ADVREGEN_1;
#endif
}

/**
 * @brief   Enables the ADC analog circuit.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 */
static void adc_lld_analog_on(ADCDriver *adcp) {

  adcp->adcm->CR = ADC_CR_ADEN;
  while ((adcp->adcm->ISR & ADC_ISR_ADRDY) == 0)
    ;
#if STM32_ADC_DUAL_MODE
  adcp->adcs->CR = ADC_CR_ADEN;
  while ((adcp->adcs->ISR & ADC_ISR_ADRDY) == 0)
    ;
#endif
}

/**
 * @brief   Disables the ADC  analog circuit.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 */
static void adc_lld_analog_off(ADCDriver *adcp) {

  adcp->adcm->CR = ADC_CR_ADDIS;
  while ((adcp->adcm->CR & ADC_CR_ADDIS) != 0)
    ;
#if STM32_ADC_DUAL_MODE
  adcp->adcs->CR = ADC_CR_ADDIS;
  while ((adcp->adcs->CR & ADC_CR_ADDIS) != 0)
    ;
#endif
}

/**
 * @brief   Calibrates and ADC unit.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 */
static void adc_lld_calibrate(ADCDriver *adcp) {

  chDbgAssert(adcp->adcm->CR == 0, "adc_lld_calibrate(), #1",
                                   "invalid register state");
  adcp->adcm->CR |= ADC_CR_ADCAL;
  while ((adcp->adcm->CR & ADC_CR_ADCAL) != 0)
    ;
#if STM32_ADC_DUAL_MODE
  chDbgAssert(adcp->adcs->CR == 0, "adc_lld_calibrate(), #2",
                                   "invalid register state");
  adcp->adcs->CR |= ADC_CR_ADCAL;
  while ((adcp->adcs->CR & ADC_CR_ADCAL) != 0)
    ;
#endif
}

/**
 * @brief   Stops an ongoing conversion, if any.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 */
static void adc_lld_stop_adc(ADCDriver *adcp) {

  if (adcp->adcm->CR & ADC_CR_ADSTART) {
    adcp->adcm->CR |= ADC_CR_ADSTP;
    while (adcp->adcm->CR & ADC_CR_ADSTP)
      ;
  }
#if STM32_ADC_DUAL_MODE
  if (adcp->adcs->CR & ADC_CR_ADSTART) {
    adcp->adcs->CR |= ADC_CR_ADSTP;
    while (adcp->adcs->CR & ADC_CR_ADSTP)
      ;
  }
#endif
}

/**
 * @brief   ADC DMA ISR service routine.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 * @param[in] flags     pre-shifted content of the ISR register
 */
static void adc_lld_serve_dma_interrupt(ADCDriver *adcp, uint32_t flags) {

  /* DMA errors handling.*/
  if ((flags & (STM32_DMA_ISR_TEIF | STM32_DMA_ISR_DMEIF)) != 0) {
    /* DMA, this could help only if the DMA tries to access an unmapped
       address space or violates alignment rules.*/
    _adc_isr_error_code(adcp, ADC_ERR_DMAFAILURE);
  }
  else {
    /* It is possible that the conversion group has already be reset by the
       ADC error handler, in this case this interrupt is spurious.*/
    if (adcp->grpp != NULL) {
      if ((flags & STM32_DMA_ISR_HTIF) != 0) {
        /* Half transfer processing.*/
        _adc_isr_half_code(adcp);
      }
      if ((flags & STM32_DMA_ISR_TCIF) != 0) {
        /* Transfer complete processing.*/
        _adc_isr_full_code(adcp);
      }
    }
  }
}

/**
 * @brief   ADC ISR service routine.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 * @param[in] isr     pre-shifted content of the ISR register
 */
static void adc_lld_serve_interrupt(ADCDriver *adcp, uint32_t isr) {

  /* It could be a spurious interrupt caused by overflows after DMA disabling,
     just ignore it in this case.*/
  if (adcp->grpp != NULL) {
    /* Note, an overflow may occur after the conversion ended before the driver
       is able to stop the ADC, this is why the DMA channel is checked too.*/
    if ((isr & ADC_ISR_OVR) &&
        (dmaStreamGetTransactionSize(adcp->dmastp) > 0)) {
      /* ADC overflow condition, this could happen only if the DMA is unable
         to read data fast enough.*/
      _adc_isr_error_code(adcp, ADC_ERR_OVERFLOW);
    }
    if (isr & ADC_ISR_AWD1) {
      /* Analog watchdog error.*/
      _adc_isr_error_code(adcp, ADC_ERR_AWD1);
    }
    if (isr & ADC_ISR_AWD2) {
      /* Analog watchdog error.*/
      _adc_isr_error_code(adcp, ADC_ERR_AWD2);
    }
    if (isr & ADC_ISR_AWD3) {
      /* Analog watchdog error.*/
      _adc_isr_error_code(adcp, ADC_ERR_AWD3);
    }
  }
}

/*===========================================================================*/
/* Driver interrupt handlers.                                                */
/*===========================================================================*/

#if STM32_ADC_USE_ADC1 || defined(__DOXYGEN__)
/**
 * @brief   ADC1/ADC2 interrupt handler.
 *
 * @isr
 */
CH_IRQ_HANDLER(Vector88) {
  uint32_t isr;

  CH_IRQ_PROLOGUE();

#if STM32_ADC_DUAL_MODE
  isr  = ADC1->ISR;
  isr |= ADC2->ISR;
  ADC1->ISR = isr;
  ADC2->ISR = isr;
#else /* !STM32_ADC_DUAL_MODE */
  isr  = ADC1->ISR;
  ADC1->ISR = isr;
#endif /* !STM32_ADC_DUAL_MODE */

  adc_lld_serve_interrupt(&ADCD1, isr);

  CH_IRQ_EPILOGUE();
}
#endif /* STM32_ADC_USE_ADC1 */

#if STM32_ADC_USE_ADC3 || defined(__DOXYGEN__)
/**
 * @brief   ADC3 interrupt handler.
 *
 * @isr
 */
CH_IRQ_HANDLER(VectorFC) {
  uint32_t isr;

  CH_IRQ_PROLOGUE();

  isr  = ADC3->ISR;
  ADC3->ISR = isr;

  adc_lld_serve_interrupt(&ADCD3, isr);

  CH_IRQ_EPILOGUE();
}

#if STM32_ADC_DUAL_MODE
/**
 * @brief   ADC4 interrupt handler (as ADC3 slave).
 *
 * @isr
 */
CH_IRQ_HANDLER(Vector134) {
  uint32_t isr;

  CH_IRQ_PROLOGUE();

  isr  = ADC4->ISR;
  ADC4->ISR = isr;

  adc_lld_serve_interrupt(&ADCD3, isr);

  CH_IRQ_EPILOGUE();
}
#endif /* STM32_ADC_DUAL_MODE */
#endif /* STM32_ADC_USE_ADC3 */

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Low level ADC driver initialization.
 *
 * @notapi
 */
void adc_lld_init(void) {

#if STM32_ADC_USE_ADC1
  /* Driver initialization.*/
  adcObjectInit(&ADCD1);
  ADCD1.adcm = ADC1;
#if STM32_ADC_DUAL_MODE
  ADCD1.adcs = ADC2;
#endif
  ADCD1.dmastp  = STM32_DMA1_STREAM1;
  ADCD1.dmamode = STM32_DMA_CR_PL(STM32_ADC_ADC12_DMA_PRIORITY) |
                  STM32_DMA_CR_DIR_P2M |
                  STM32_DMA_CR_MSIZE_HWORD | STM32_DMA_CR_PSIZE_HWORD |
                  STM32_DMA_CR_MINC        | STM32_DMA_CR_TCIE        |
                  STM32_DMA_CR_DMEIE       | STM32_DMA_CR_TEIE;
  nvicEnableVector(ADC1_2_IRQn,
                   CORTEX_PRIORITY_MASK(STM32_ADC_ADC12_IRQ_PRIORITY));
#endif /* STM32_ADC_USE_ADC1 */

#if STM32_ADC_USE_ADC3
  /* Driver initialization.*/
  adcObjectInit(&ADCD1);
  ADCD3.adcm = ADC3;
#if STM32_ADC_DUAL_MODE
  ADCD3.adcs = ADC4;
#endif
  ADCD3.dmastp  = STM32_DMA2_STREAM5;
  ADCD3.dmamode = STM32_DMA_CR_PL(STM32_ADC_ADC12_DMA_PRIORITY) |
                  STM32_DMA_CR_DIR_P2M |
                  STM32_DMA_CR_MSIZE_HWORD | STM32_DMA_CR_PSIZE_HWORD |
                  STM32_DMA_CR_MINC        | STM32_DMA_CR_TCIE        |
                  STM32_DMA_CR_DMEIE       | STM32_DMA_CR_TEIE;
  nvicEnableVector(ADC3_IRQn,
                   CORTEX_PRIORITY_MASK(STM32_ADC_ADC34_IRQ_PRIORITY));
#if STM32_ADC_DUAL_MODE
  nvicEnableVector(ADC4_IRQn,
                   CORTEX_PRIORITY_MASK(STM32_ADC_ADC34_IRQ_PRIORITY));
#endif
#endif /* STM32_ADC_USE_ADC3 */
}

/**
 * @brief   Configures and activates the ADC peripheral.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 *
 * @notapi
 */
void adc_lld_start(ADCDriver *adcp) {

  /* If in stopped state then enables the ADC and DMA clocks.*/
  if (adcp->state == ADC_STOP) {
#if STM32_ADC_USE_ADC1
    if (&ADCD1 == adcp) {
      bool_t b;
      b = dmaStreamAllocate(adcp->dmastp,
                            STM32_ADC_ADC12_DMA_IRQ_PRIORITY,
                            (stm32_dmaisr_t)adc_lld_serve_dma_interrupt,
                            (void *)adcp);
      chDbgAssert(!b, "adc_lld_start(), #1", "stream already allocated");
#if STM32_ADC_DUAL_MODE
      dmaStreamSetPeripheral(adcp->dmastp, &ADC1_2->CDR);
#else
      dmaStreamSetPeripheral(adcp->dmastp, &ADC1->DR);
#endif
      rccEnableADC12(FALSE);

      /* Clock source setting.*/
      ADC1_2->CCR = ADC_CCR_CKMODE_AHB_DIV1;
    }
#endif /* STM32_ADC_USE_ADC1 */

#if STM32_ADC_USE_ADC3
    if (&ADCD3 == adcp) {
      bool_t b;
      b = dmaStreamAllocate(adcp->dmastp,
                            STM32_ADC_ADC34_DMA_IRQ_PRIORITY,
                            (stm32_dmaisr_t)adc_lld_serve_dma_interrupt,
                            (void *)adcp);
      chDbgAssert(!b, "adc_lld_start(), #2", "stream already allocated");
#if STM32_ADC_DUAL_MODE
      dmaStreamSetPeripheral(adcp->dmastp, &ADC3_4->CDR);
#else
      dmaStreamSetPeripheral(adcp->dmastp, &ADC3->DR);
#endif
      rccEnableADC34(FALSE);

      /* Clock source setting.*/
      ADC3_4->CCR = ADC_CCR_CKMODE_AHB_DIV1;
    }
#endif /* STM32_ADC_USE_ADC2 */

    /* Master ADC calibration.*/
    adc_lld_vreg_on(adcp);
    adc_lld_calibrate(adcp);

    /* Master ADC enabled here in order to reduce conversions latencies.*/
    adc_lld_analog_on(adcp);
  }
}

/**
 * @brief   Deactivates the ADC peripheral.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 *
 * @notapi
 */
void adc_lld_stop(ADCDriver *adcp) {

  /* If in ready state then disables the ADC clock and analog part.*/
  if (adcp->state == ADC_READY) {

    /* Releasing the associated DMA channel.*/
    dmaStreamRelease(adcp->dmastp);

    /* Disabling the ADC.*/
    if (adcp->adcm->CR & ADC_CR_ADEN) {
      /* Stopping the ongoing conversion, if any.*/
      adc_lld_stop_adc(adcp);

      /* Disabling ADC analog circuit and regulator.*/
      adc_lld_analog_off(adcp);
      adc_lld_vreg_off(adcp);
    }

#if STM32_ADC_USE_ADC1
    if (&ADCD1 == adcp)
      rccDisableADC12(FALSE);
#endif

#if STM32_ADC_USE_ADC3
    if (&ADCD1 == adcp)
      rccDisableADC34(FALSE);
#endif
  }
}

/**
 * @brief   Starts an ADC conversion.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 *
 * @notapi
 */
void adc_lld_start_conversion(ADCDriver *adcp) {
  uint32_t mode;
  const ADCConversionGroup *grpp = adcp->grpp;

  /* DMA setup.*/
  mode = adcp->dmamode;
  if (grpp->circular) {
    mode |= STM32_DMA_CR_CIRC;
  }
  if (adcp->depth > 1) {
    /* If the buffer depth is greater than one then the half transfer interrupt
       interrupt is enabled in order to allows streaming processing.*/
    mode |= STM32_DMA_CR_HTIE;
  }
  dmaStreamSetMemory0(adcp->dmastp, adcp->samples);
  dmaStreamSetTransactionSize(adcp->dmastp, (uint32_t)grpp->num_channels *
                                            (uint32_t)adcp->depth);
  dmaStreamSetMode(adcp->dmastp, mode);
  dmaStreamEnable(adcp->dmastp);

  /* ADC setup, if it is defined a callback for the analog watch dog then it
     is enabled.*/
  adcp->adc->ISR    = adcp->adc->ISR;
  adcp->adc->IER    = ADC_IER_OVRIE | ADC_IER_AWDIE;
  adcp->adc->TR     = grpp->tr;
  adcp->adc->SMPR   = grpp->smpr;
  adcp->adc->CHSELR = grpp->chselr;

  /* ADC configuration and start.*/
  adcp->adc->CFGR1  = grpp->cfgr1 | ADC_CFGR1_CONT  | ADC_CFGR1_DMACFG |
                                    ADC_CFGR1_DMAEN;
  adcp->adc->CR    |= ADC_CR_ADSTART;
}

/**
 * @brief   Stops an ongoing conversion.
 *
 * @param[in] adcp      pointer to the @p ADCDriver object
 *
 * @notapi
 */
void adc_lld_stop_conversion(ADCDriver *adcp) {

  dmaStreamDisable(adcp->dmastp);
  adc_lld_stop_adc(adcp->adc);
}

/**
 * @brief   Programs the analog watchdog 2.
 * @note    This function must be called after starting the driver and
 *          before starting a conversion.
 *
 * @param[in] adc       pointer to the physical ADC to configure
 * @param[in] low       lower limit, as a 12 bits value
 * @param[in] high      upper limit, as a 12 bits value
 * @param[in] channels  bit mask of guarded channels
 *
 * @api
 */
void adcSTM32SetWatchdog2(ADC_TypeDef *adc, uint16_t low, uint16_t high,
                          uint32_t channels) {

}

/**
 * @brief   Programs the analog watchdog 3.
 * @note    This function must be called after starting the driver and
 *          before starting a conversion.
 *
 * @param[in] adc       pointer to the physical ADC to configure
 * @param[in] low       lower limit, as a 12 bits value
 * @param[in] high      upper limit, as a 12 bits value
 * @param[in] channels  bit mask of guarded channels
 *
 * @api
 */
void adcSTM32SetWatchdog3(ADC_TypeDef *adc, uint16_t low, uint16_t high,
                          uint32_t channels) {

}

#endif /* HAL_USE_ADC */

/** @} */