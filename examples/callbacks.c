/* HAL callback yönlendirmeleri (INTEGRATION.md §4).
 * Bu dosya örnektir, derlemeye EKLENMEZ. Projede aynı isimli fonksiyonlar
 * zaten varsa yenisini yazma: yalnız ilgili satırı mevcut olanın BAŞINA ekle
 * (özellikle erken `return` eden filtrelerin üstüne). */
#include "BTM9011EP.h"

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    BTM_OnSpiComplete(hspi);
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    BTM_OnSpiError(hspi);
}

/* HAL timebase'i de bu callback'i kullanır (TIM6/TIM7); sürücünün satırı
 * timebase kontrolünün DIŞINA konur. Sürücü yabancı timer'ları yok sayar. */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    BTM_OnTimerPeriod(htim);
    /* if (htim->Instance == TIM6) { HAL_IncTick(); } */
}

void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
    BTM_OnTimerCompare(htim);
}
