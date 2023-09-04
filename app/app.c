/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>
#include "app/action.h"
#include "app/aircopy.h"
#include "app/app.h"
#include "app/dtmf.h"
#include "app/fm.h"
#include "app/generic.h"
#include "app/main.h"
#include "app/menu.h"
#include "app/scanner.h"
#include "app/uart.h"
#include "ARMCM0.h"
#include "audio.h"
#include "board.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/backlight.h"
#include "driver/bk1080.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/system.h"
#include "dtmf.h"
#include "external/printf/printf.h"
#include "frequencies.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "sram-overlay.h"
#include "ui/battery.h"
#include "ui/inputbox.h"
#include "ui/menu.h"
#include "ui/rssi.h"
#include "ui/status.h"
#include "ui/ui.h"

static void APP_ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);

static void FUN_00005144(void)
{
	if (!g_SquelchLost) {
		return;
	}
	if (gStepDirection == 0) {
		if (gCssScanMode != CSS_SCAN_MODE_OFF && gRxReceptionMode == RX_MODE_NONE) {
			ScanPauseDelayIn10msec = 100;
			gScheduleScanListen = false;
			gRxReceptionMode = RX_MODE_DETECTED;
		}
		if (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF) {
			if (gIsNoaaMode) {
				gNOAA_Countdown = 20;
				gScheduleNOAA = false;
			}
			FUNCTION_Select(FUNCTION_3);
			return;
		}
		if (gRxReceptionMode != RX_MODE_NONE) {
			FUNCTION_Select(FUNCTION_3);
			return;
		}
		gDualWatchCountdown = 100;
		gScheduleDualWatch = false;
	} else {
		if (gRxReceptionMode != RX_MODE_NONE) {
			FUNCTION_Select(FUNCTION_3);
			return;
		}
		ScanPauseDelayIn10msec = 20;
		gScheduleScanListen = false;
	}
	gRxReceptionMode = RX_MODE_DETECTED;
	FUNCTION_Select(FUNCTION_3);
}

void FUN_000051e8(void)
{
	bool bFlag;

	if (!g_SquelchLost) {
		FUNCTION_Select(FUNCTION_0);
		gUpdateDisplay = true;
		return;
	}

	bFlag = (gStepDirection == 0 && gCopyOfCodeType == CODE_TYPE_OFF);
	if (gRxVfo->CHANNEL_SAVE >= NOAA_CHANNEL_FIRST && gSystickCountdown2) {
		bFlag = true;
		gSystickCountdown2 = 0;
	}
	if (g_CTCSS_Lost && gCopyOfCodeType == CODE_TYPE_CONTINUOUS_TONE) {
		bFlag = true;
		gFoundCTCSS = false;
	}
	if (g_CDCSS_Lost && gCDCSSCodeType == CDCSS_POSITIVE_CODE && (gCopyOfCodeType == CODE_TYPE_DIGITAL || gCopyOfCodeType == CODE_TYPE_REVERSE_DIGITAL)) {
		gFoundCDCSS = false;
	} else if (!bFlag) {
		return;
	}

	DTMF_HandleRequest();

	if (gStepDirection == 0 && gCssScanMode == CSS_SCAN_MODE_OFF) {
		if (gRxVfo->DTMF_DECODING_ENABLE || gSetting_KILLED) {
			if (gDTMF_CallState == DTMF_CALL_STATE_NONE) {
				if (gRxReceptionMode == RX_MODE_DETECTED) {
					gDualWatchCountdown = 500;
					gScheduleDualWatch = false;
					gRxReceptionMode = RX_MODE_LISTENING;
					return;
				}
			}
		}
	}

	APP_StartListening(FUNCTION_RECEIVE);
}

void FUN_0000773c(void)
{
	uint8_t Previous;

	Previous = gRestoreMrChannel;
	gStepDirection = 0;

	if (!g_20000413) {
		if (IS_MR_CHANNEL(gNextMrChannel)) {
			gEeprom.MrChannel[gEeprom.RX_CHANNEL] = gRestoreMrChannel;
			gEeprom.ScreenChannel[gEeprom.RX_CHANNEL] = Previous;
			RADIO_ConfigureChannel(gEeprom.RX_CHANNEL, 2);
		} else {
			gRxVfo->ConfigRX.Frequency = gRestoreFrequency;
			RADIO_ApplyOffset(gRxVfo);
			RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
		}
		RADIO_SetupRegisters(true);
		gUpdateDisplay = true;
		return;
	}

	if (!IS_MR_CHANNEL(gRxVfo->CHANNEL_SAVE)) {
		RADIO_ApplyOffset(gRxVfo);
		RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
		SETTINGS_SaveChannel(gRxVfo->CHANNEL_SAVE, gEeprom.RX_CHANNEL, gRxVfo, 1);
		return;
	}

	SETTINGS_SaveVfoIndices();
}

void FUN_000052f0(void)
{
	uint8_t Value;

	Value = 0;

	if (gSystickFlag10) {
		Value = 1;
		goto Skip;
	} else if (gStepDirection && IS_FREQ_CHANNEL(gNextMrChannel)) {
		if (g_SquelchLost) {
			return;
		}
		Value = 1;
		goto Skip;
	}
	switch (gCopyOfCodeType) {
	case CODE_TYPE_CONTINUOUS_TONE:
		if (gFoundCTCSS && gFoundCTCSSCountdown == 0) {
			gFoundCTCSS = false;
			gFoundCDCSS = false;
			Value = 1;
			goto Skip;
		}
		break;
	case CODE_TYPE_DIGITAL:
	case CODE_TYPE_REVERSE_DIGITAL:
		if (gFoundCDCSS && gFoundCDCSSCountdown == 0) {
			gFoundCTCSS = false;
			gFoundCDCSS = false;
			Value = 1;
			goto Skip;
		}
		break;
	default:
		break;
	}

	if (g_SquelchLost) {
		if (!gEndOfRxDetectedMaybe && IS_NOT_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE)) {
			switch (gCopyOfCodeType) {
			case CODE_TYPE_OFF:
				if (gEeprom.SQUELCH_LEVEL) {
					if (g_CxCSS_TAIL_Found) {
						Value = 2;
						g_CxCSS_TAIL_Found = false;
					}
				}
				break;

			case CODE_TYPE_CONTINUOUS_TONE:
				if (g_CTCSS_Lost) {
					gFoundCTCSS = false;
				} else if (!gFoundCTCSS) {
					gFoundCTCSS = true;
					gFoundCTCSSCountdown = 100;
				}
				if (g_CxCSS_TAIL_Found) {
					Value = 2;
					g_CxCSS_TAIL_Found = false;
				}
				break;

			case CODE_TYPE_DIGITAL:
			case CODE_TYPE_REVERSE_DIGITAL:
				if (g_CDCSS_Lost && gCDCSSCodeType == CDCSS_POSITIVE_CODE) {
					gFoundCDCSS = false;
				} else if (!gFoundCDCSS) {
					gFoundCDCSS = true;
					gFoundCDCSSCountdown = 100;
				}
				if (g_CxCSS_TAIL_Found) {
					if (BK4819_GetCTCType() == 1) {
						Value = 2;
					}
					g_CxCSS_TAIL_Found = false;
				}
				break;

			default:
				break;
			}
		}
	} else {
		Value = 1;
	}

	if (!gEndOfRxDetectedMaybe && !Value && gNextTimeslice40ms && gEeprom.TAIL_NOTE_ELIMINATION && (gCopyOfCodeType == CODE_TYPE_DIGITAL || gCopyOfCodeType == CODE_TYPE_REVERSE_DIGITAL) && BK4819_GetCTCType() == 1) {
		Value = 2;
	} else {
		gNextTimeslice40ms = false;
	}

Skip:
	switch (Value) {
	case 1:
		RADIO_SetupRegisters(true);
		if (IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE)) {
			gSystickCountdown2 = 300;
		}
		gUpdateDisplay = true;
		if (gStepDirection) {
			switch (gEeprom.SCAN_RESUME_MODE) {
			case SCAN_RESUME_CO:
				ScanPauseDelayIn10msec = 360;
				gScheduleScanListen = false;
				break;
			case SCAN_RESUME_SE:
				FUN_0000773c();
				break;
			}
		}
		break;
	case 2:
		if (gEeprom.TAIL_NOTE_ELIMINATION) {
			GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);
			g_20000342 = 20;
			gSystickFlag10 = false;
			gEnableSpeaker = false;
			gEndOfRxDetectedMaybe = true;
		}
		break;
	}
}

void FUN_0000510c(void)
{
	switch (gCurrentFunction) {
	case FUNCTION_0:
		FUN_00005144();
		break;
	case FUNCTION_POWER_SAVE:
		if (!gRxIdleMode) {
			FUN_00005144();
		}
		break;
	case FUNCTION_3:
		FUN_000051e8();
		break;
	case FUNCTION_RECEIVE:
		FUN_000052f0();
		break;
	default:
		break;
	}
}

void APP_StartListening(FUNCTION_Type_t Function)
{
	if (!gSetting_KILLED) {
		if (gFmRadioMode) {
			BK1080_Init(0, false);
		}
		gVFO_RSSI_Level[gEeprom.RX_CHANNEL == 0] = 0;
		GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);
		gEnableSpeaker = true;
		BACKLIGHT_TurnOn();
		if (gStepDirection) {
			switch (gEeprom.SCAN_RESUME_MODE) {
			case SCAN_RESUME_TO:
				if (!gScanPauseMode) {
					ScanPauseDelayIn10msec = 500;
					gScheduleScanListen = false;
					gScanPauseMode = true;
				}
				break;
			case SCAN_RESUME_CO:
			case SCAN_RESUME_SE:
				ScanPauseDelayIn10msec = 0;
				gScheduleScanListen = false;
				break;
			}
			g_20000413 = true;
		}
		if (IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE) && gIsNoaaMode) {
			gRxVfo->CHANNEL_SAVE = gNoaaChannel + NOAA_CHANNEL_FIRST;
			gRxVfo->pCurrent->Frequency = NoaaFrequencyTable[gNoaaChannel];
			gRxVfo->pReverse->Frequency = NoaaFrequencyTable[gNoaaChannel];
			gEeprom.ScreenChannel[gEeprom.RX_CHANNEL] = gRxVfo->CHANNEL_SAVE;
			gNOAA_Countdown = 500;
			gScheduleNOAA = false;
		}
		if (gCssScanMode != CSS_SCAN_MODE_OFF) {
			gCssScanMode = CSS_SCAN_MODE_FOUND;
		}
		if ((gStepDirection == 0 && gCssScanMode == CSS_SCAN_MODE_OFF) && gEeprom.DUAL_WATCH != DUAL_WATCH_OFF) {
			g_2000041F = 1;
			gDualWatchCountdown = 360;
			gScheduleDualWatch = false;
		}
		if (gRxVfo->IsAM) {
			BK4819_WriteRegister(BK4819_REG_48, 0xB3A8);
			g_20000474 = 0;
		} else {
			BK4819_WriteRegister(BK4819_REG_48, 0xB000
					| (gEeprom.VOLUME_GAIN << 4)
					| (gEeprom.DAC_GAIN << 0)
					);
		}
		if (gVoiceWriteIndex == 0) {
			if (gRxVfo->IsAM) {
				BK4819_SetAF(BK4819_AF_AM);
			} else {
				BK4819_SetAF(BK4819_AF_OPEN);
			}
		}
		FUNCTION_Select(Function);
		if (Function == FUNCTION_MONITOR || gFmRadioMode) {
			GUI_SelectNextDisplay(DISPLAY_MAIN);
			return;
		}
		gUpdateDisplay = true;
	}
}

void APP_SetFrequencyByStep(VFO_Info_t *pInfo, int8_t Step)
{
	uint32_t Frequency;

	Frequency = pInfo->ConfigRX.Frequency + (Step * pInfo->StepFrequency);
	if (Frequency >= gUpperLimitFrequencyBandTable[pInfo->Band]) {
		pInfo->ConfigRX.Frequency = gLowerLimitFrequencyBandTable[pInfo->Band];
	} else if (Frequency < gLowerLimitFrequencyBandTable[pInfo->Band]) {
		pInfo->ConfigRX.Frequency = FREQUENCY_FloorToStep(gUpperLimitFrequencyBandTable[pInfo->Band], pInfo->StepFrequency, gLowerLimitFrequencyBandTable[pInfo->Band]);
	} else {
		pInfo->ConfigRX.Frequency = Frequency;
	}
}

void APP_MoreRadioStuff(void)
{
	APP_SetFrequencyByStep(gRxVfo, gStepDirection);
	RADIO_ApplyOffset(gRxVfo);
	RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
	RADIO_SetupRegisters(true);
	gUpdateDisplay = true;
	ScanPauseDelayIn10msec = 10;
	g_20000413 = false;
}

void FUN_00007dd4(void)
{
	uint8_t Ch1 = gEeprom.SCANLIST_PRIORITY_CH1[gEeprom.SCAN_LIST_DEFAULT];
	uint8_t Ch2 = gEeprom.SCANLIST_PRIORITY_CH2[gEeprom.SCAN_LIST_DEFAULT];
	uint8_t PreviousCh, Ch;
	bool bEnabled;

	PreviousCh = gNextMrChannel;
	bEnabled = gEeprom.SCAN_LIST_ENABLED[gEeprom.SCAN_LIST_DEFAULT];
	if (bEnabled) {
		if (gCurrentScanList == 0) {
			gPreviousMrChannel = gNextMrChannel;
			if (RADIO_CheckValidChannel(Ch1, false, 0)) {
				gNextMrChannel = Ch1;
			} else {
				gCurrentScanList = 1;
			}
		}
		if (gCurrentScanList == 1) {
			if (RADIO_CheckValidChannel(Ch2, false, 0)) {
				gNextMrChannel = Ch2;
			} else {
				gCurrentScanList = 2;
			}
		}
		if (gCurrentScanList == 2) {
			gNextMrChannel = gPreviousMrChannel;
		} else {
			goto Skip;
		}
	}

	Ch = RADIO_FindNextChannel(gNextMrChannel + gStepDirection, gStepDirection, true, gEeprom.SCAN_LIST_DEFAULT);
	if (Ch == 0xFF) {
		return;
	}

	gNextMrChannel = Ch;

Skip:
	if (PreviousCh != gNextMrChannel) {
		gEeprom.MrChannel[gEeprom.RX_CHANNEL] = gNextMrChannel;
		gEeprom.ScreenChannel[gEeprom.RX_CHANNEL] = gNextMrChannel;
		RADIO_ConfigureChannel(gEeprom.RX_CHANNEL, 2);
		RADIO_SetupRegisters(true);
		gUpdateDisplay = true;
	}
	ScanPauseDelayIn10msec = 20;
	g_20000413 = false;
	if (bEnabled) {
		gCurrentScanList++;
		if (gCurrentScanList >= 2) {
			gCurrentScanList = 0;
		}
	}
}

void NOAA_IncreaseChannel(void)
{
	gNoaaChannel++;
	if (gNoaaChannel > 9) {
		gNoaaChannel = 0;
	}
}

static void DUALWATCH_Alternate(void)
{
	if (gIsNoaaMode) {
		if (IS_NOT_NOAA_CHANNEL(gEeprom.ScreenChannel[0]) || IS_NOT_NOAA_CHANNEL(gEeprom.ScreenChannel[1])) {
			gEeprom.RX_CHANNEL = gEeprom.RX_CHANNEL == 0;
		} else {
			gEeprom.RX_CHANNEL = 0;
		}
		gRxVfo = &gEeprom.VfoInfo[gEeprom.RX_CHANNEL];
		if (gEeprom.VfoInfo[0].CHANNEL_SAVE >= NOAA_CHANNEL_FIRST) {
			NOAA_IncreaseChannel();
		}
	} else {
		gEeprom.RX_CHANNEL = gEeprom.RX_CHANNEL == 0;
		gRxVfo = &gEeprom.VfoInfo[gEeprom.RX_CHANNEL];
	}
	RADIO_SetupRegisters(false);
	if (gIsNoaaMode) {
		gDualWatchCountdown = 7;
	} else {
		gDualWatchCountdown = 10;
	}
}

void APP_CheckRadioInterrupts(void)
{
	if (gScreenToDisplay == DISPLAY_SCANNER) {
		return;
	}

	while (BK4819_GetRegister(BK4819_REG_0C) & 1U) {
		uint16_t Mask;

		BK4819_WriteRegister(BK4819_REG_02, 0);
		Mask = BK4819_GetRegister(BK4819_REG_02);
		if (Mask & BK4819_REG_02_DTMF_5TONE_FOUND) {
			gDTMF_RequestPending = true;
			gDTMF_RecvTimeout = 5;
			if (gDTMF_WriteIndex > 15) {
				uint8_t i;
				for (i = 0; i < sizeof(gDTMF_Received) - 1; i++) {
					gDTMF_Received[i] = gDTMF_Received[i + 1];
				}
				gDTMF_WriteIndex = 15;
			}
			gDTMF_Received[gDTMF_WriteIndex++] = DTMF_GetCharacter(BK4819_GetDTMF_5TONE_Code());
			if (gCurrentFunction == FUNCTION_RECEIVE) {
				DTMF_HandleRequest();
			}
		}
		if (Mask & BK4819_REG_02_CxCSS_TAIL) {
			g_CxCSS_TAIL_Found = true;
		}
		if (Mask & BK4819_REG_02_CDCSS_LOST) {
			g_CDCSS_Lost = true;
			gCDCSSCodeType = BK4819_GetCDCSSCodeType();
		}
		if (Mask & BK4819_REG_02_CDCSS_FOUND) {
			g_CDCSS_Lost = false;
		}
		if (Mask & BK4819_REG_02_CTCSS_LOST) {
			g_CTCSS_Lost = true;
		}
		if (Mask & BK4819_REG_02_CTCSS_FOUND) {
			g_CTCSS_Lost = false;
		}
		if (Mask & BK4819_REG_02_VOX_LOST) {
			g_VOX_Lost = true;
			gVoxPauseCountdown = 10;
			if (gEeprom.VOX_SWITCH) {
				if (gCurrentFunction == FUNCTION_POWER_SAVE && !gRxIdleMode) {
					gBatterySave = 20;
					gBatterySaveCountdownExpired = 0;
				}
				if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF && (gScheduleDualWatch || gDualWatchCountdown < 20)) {
					gDualWatchCountdown = 20;
					gScheduleDualWatch = false;
				}
			}
		}
		if (Mask & BK4819_REG_02_VOX_FOUND) {
			g_VOX_Lost = false;
			gVoxPauseCountdown = 0;
		}
		if (Mask & BK4819_REG_02_SQUELCH_LOST) {
			g_SquelchLost = true;
			BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_GREEN, true);
		}
		if (Mask & BK4819_REG_02_SQUELCH_FOUND) {
			g_SquelchLost = false;
			BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_GREEN, false);
		}
		if (Mask & BK4819_REG_02_FSK_FIFO_ALMOST_FULL && gScreenToDisplay == DISPLAY_AIRCOPY && gAircopyState == AIRCOPY_TRANSFER && gAirCopyIsSendMode == 0) {
			uint8_t i;

			for (i = 0; i < 4; i++) {
				g_FSK_Buffer[gFSKWriteIndex++] = BK4819_GetRegister(BK4819_REG_5F);
			}
			AIRCOPY_StorePacket();
		}
	}
}

void TalkRelatedCode(void)
{
	RADIO_SendEndOfTransmission();
	RADIO_EnableCxCSS();
	RADIO_SetupRegisters(false);
}

static void FUN_00008334(void)
{
	if (!gSetting_KILLED) {
		if (g_200003B6 == 0) {
			if (gVoxPauseCountdown) {
				return;
			}
		} else {
			g_VOX_Lost = false;
			gVoxPauseCountdown = 0;
		}
		if (gCurrentFunction != FUNCTION_RECEIVE && gCurrentFunction != FUNCTION_MONITOR && gStepDirection == 0 && gCssScanMode == CSS_SCAN_MODE_OFF && !gFmRadioMode) {
			if (gVOX_NoiseDetected) {
				if (g_VOX_Lost) {
					gVoxStopCountdown = 100;
				} else if (gVoxStopCountdown == 0) {
					gVOX_NoiseDetected = false;
				}
				if (gCurrentFunction == FUNCTION_TRANSMIT && !gPttIsPressed && !gVOX_NoiseDetected) {
					if (gFlagEndTransmission) {
						FUNCTION_Select(FUNCTION_0);
					} else {
						TalkRelatedCode();
						if (gEeprom.REPEATER_TAIL_TONE_ELIMINATION == 0) {
							FUNCTION_Select(FUNCTION_0);
						} else {
							gRTTECountdown = gEeprom.REPEATER_TAIL_TONE_ELIMINATION * 10;
						}
					}
					gUpdateDisplay = true;
					gFlagEndTransmission = false;
					return;
				}
			} else if (g_VOX_Lost) {
				gVOX_NoiseDetected = true;
				if (gCurrentFunction == FUNCTION_POWER_SAVE) {
					FUNCTION_Select(FUNCTION_0);
				}
				if (gCurrentFunction != FUNCTION_TRANSMIT) {
					gDTMF_ReplyState = DTMF_REPLY_NONE;
					RADIO_PrepareTX();
					gUpdateDisplay = true;
				}
			}
		}
	}
}

void APP_Update(void)
{
	if (gFlagPlayQueuedVoice) {
		AUDIO_PlayQueuedVoice();
		gFlagPlayQueuedVoice = false;
	}

	if (gCurrentFunction == FUNCTION_TRANSMIT && gTxTimeoutReached) {
		gTxTimeoutReached = false;
		gFlagEndTransmission = true;
		TalkRelatedCode();
		AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP);
		RADIO_SetVfoState(VFO_STATE_TIMEOUT);
		GUI_DisplayScreen();
	}

	if (gReducedService) {
		return;
	}
	if (gCurrentFunction != FUNCTION_TRANSMIT) {
		FUN_0000510c();
	}
	if (gFmRadioCountdown) {
		return;
	}

	if (gScreenToDisplay != DISPLAY_SCANNER && gStepDirection && gScheduleScanListen && !gPttIsPressed && gVoiceWriteIndex == 0) {
		if (IS_FREQ_CHANNEL(gNextMrChannel)) {
			if (gCurrentFunction == FUNCTION_3) {
				APP_StartListening(FUNCTION_RECEIVE);
			} else {
				APP_MoreRadioStuff();
			}
		} else {
			if (gCopyOfCodeType == CODE_TYPE_OFF && gCurrentFunction == FUNCTION_3) {
				APP_StartListening(FUNCTION_RECEIVE);
			} else {
				FUN_00007dd4();
			}
		}
		gScanPauseMode = false;
		gRxReceptionMode = RX_MODE_NONE;
		gScheduleScanListen = false;
	}

	if (gCssScanMode == CSS_SCAN_MODE_SCANNING && gScheduleScanListen && gVoiceWriteIndex == 0) {
		MENU_SelectNextDCS();
		gScheduleScanListen = false;
	}

	if (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF && gIsNoaaMode && gScheduleNOAA && gVoiceWriteIndex == 0) {
		NOAA_IncreaseChannel();
		RADIO_SetupRegisters(false);
		gScheduleNOAA = false;
		gNOAA_Countdown = 7;
	}

	if (gScreenToDisplay != DISPLAY_SCANNER && gEeprom.DUAL_WATCH != DUAL_WATCH_OFF) {
		if (gScheduleDualWatch && gVoiceWriteIndex == 0) {
			if (gStepDirection == 0 && gCssScanMode == CSS_SCAN_MODE_OFF) {
				if (!gPttIsPressed && !gFmRadioMode && gDTMF_CallState == DTMF_CALL_STATE_NONE && gCurrentFunction != FUNCTION_POWER_SAVE) {
					DUALWATCH_Alternate();
					if (g_2000041F == 1 && gScreenToDisplay == DISPLAY_MAIN) {
						GUI_SelectNextDisplay(DISPLAY_MAIN);
					}
					g_2000041F = 0;
					gScanPauseMode = false;
					gRxReceptionMode = RX_MODE_NONE;
					gScheduleDualWatch = false;
				}
			}
		}
	}

	if (gFM_Step && gScheduleFM && gCurrentFunction != FUNCTION_MONITOR && gCurrentFunction != FUNCTION_RECEIVE && gCurrentFunction != FUNCTION_TRANSMIT) {
		FM_Play();
		gScheduleFM = false;
	}

	if (gEeprom.VOX_SWITCH) {
		FUN_00008334();
	}

	if (gSchedulePowerSave) {
		if (gEeprom.BATTERY_SAVE == 0 || gStepDirection || gCssScanMode != CSS_SCAN_MODE_OFF || gFmRadioMode || gPttIsPressed || gScreenToDisplay != DISPLAY_MAIN || gKeyBeingHeld || gDTMF_CallState != DTMF_CALL_STATE_NONE) {
			gBatterySaveCountdown = 1000;
		} else {
			if ((IS_NOT_NOAA_CHANNEL(gEeprom.ScreenChannel[0]) && IS_NOT_NOAA_CHANNEL(gEeprom.ScreenChannel[1])) || !gIsNoaaMode) {
				FUNCTION_Select(FUNCTION_POWER_SAVE);
			} else {
				gBatterySaveCountdown = 1000;
			}
		}
		gSchedulePowerSave = false;
	}

	if (gBatterySaveCountdownExpired && gCurrentFunction == FUNCTION_POWER_SAVE && gVoiceWriteIndex == 0) {
		if (gRxIdleMode) {
			BK4819_Conditional_RX_TurnOn_and_GPIO6_Enable();
			if (gEeprom.VOX_SWITCH) {
				BK4819_EnableVox(gEeprom.VOX1_THRESHOLD, gEeprom.VOX0_THRESHOLD);
			}
			if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF && gStepDirection == 0 && gCssScanMode == CSS_SCAN_MODE_OFF) {
				DUALWATCH_Alternate();
				g_20000382 = false;
			}
			FUNCTION_Init();
			gBatterySave = 10;
			gRxIdleMode = false;
		} else if (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF || gStepDirection || gCssScanMode != CSS_SCAN_MODE_OFF || g_20000382) {
			gCurrentRSSI = BK4819_GetRSSI();
			UI_UpdateRSSI(gCurrentRSSI);
			gBatterySave = gEeprom.BATTERY_SAVE * 10;
			gRxIdleMode = true;
			BK4819_DisableVox();
			BK4819_Sleep();
			BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2, false);
			// Authentic device checked removed
		} else {
			DUALWATCH_Alternate();
			g_20000382 = true;
			gBatterySave = 10;
		}
		gBatterySaveCountdownExpired = false;
	}
}

void APP_CheckKeys(void)
{
	KEY_Code_t Key;

	if (gSetting_KILLED || (gScreenToDisplay == DISPLAY_AIRCOPY && gAircopyState != AIRCOPY_READY)) {
		return;
	}

	if (gPttIsPressed) {
		if (GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT)) {
			SYSTEM_DelayMs(20);
			if (GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT)) {
				APP_ProcessKey(KEY_PTT, false, false);
				gPttIsPressed = false;
				if (gKeyReading1 != KEY_INVALID) {
					gPttWasReleased = true;
				}
			}
		}
	} else {
		if (!GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT)) {
			gPttDebounceCounter = gPttDebounceCounter + 1;
			if (gPttDebounceCounter > 4) {
				gPttIsPressed = true;
				APP_ProcessKey(KEY_PTT, true, false);
			}
		} else {
			gPttDebounceCounter = 0;
		}
	}
	Key = KEYBOARD_Poll();
	if (gKeyReading0 != Key) {
		if (gKeyReading0 != KEY_INVALID && Key != KEY_INVALID) {
			APP_ProcessKey(gKeyReading1, false, gKeyBeingHeld);
		}
		gKeyReading0 = Key;
		gDebounceCounter = 0;
		return;
	}
	gDebounceCounter++;
	if (gDebounceCounter == 2) {
		if (Key == KEY_INVALID) {
			if (gKeyReading1 != KEY_INVALID) {
				APP_ProcessKey(gKeyReading1, false, gKeyBeingHeld);
				gKeyReading1 = KEY_INVALID;
			}
		} else {
			gKeyReading1 = Key;
			APP_ProcessKey(Key, true, false);
		}
		gKeyBeingHeld = false;
	} else if (gDebounceCounter == 128) {
		if (Key == KEY_STAR || Key == KEY_F || Key == KEY_SIDE2 || Key == KEY_SIDE1 || Key == KEY_UP || Key == KEY_DOWN) {
			gKeyBeingHeld = true;
			APP_ProcessKey(Key, true, true);
		}
	} else if (gDebounceCounter > 128) {
		if (Key == KEY_UP || Key == KEY_DOWN) {
			gKeyBeingHeld = true;
			if ((gDebounceCounter & 15) == 0) {
				APP_ProcessKey(Key, true, true);
			}
		}
		if (gDebounceCounter != 0xFFFF) {
			return;
		}
		gDebounceCounter = 128;
	}
}

void APP_TimeSlice10ms(void)
{
	gFlashLightBlinkCounter++;

	if (UART_IsCommandAvailable()) {
		__disable_irq();
		UART_HandleCommand();
		__enable_irq();
	}

	if (gReducedService) {
		return;
	}

	if (gCurrentFunction != FUNCTION_POWER_SAVE || !gRxIdleMode) {
		APP_CheckRadioInterrupts();
	}

	if (gCurrentFunction != FUNCTION_TRANSMIT) {
		if (gUpdateStatus) {
			UI_DisplayStatus();
			gUpdateStatus = false;
		}
		if (gUpdateDisplay) {
			GUI_DisplayScreen();
			gUpdateDisplay = false;
		}
	}

	// Skipping authentic device checks

	if (gFmRadioCountdown) {
		return;
	}

	if (gFlashLightState == FLASHLIGHT_BLINK && (gFlashLightBlinkCounter & 15U) == 0) {
		GPIO_FlipBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
	}
	if (g_200003B6) {
		g_200003B6--;
	}
	if (gVoxPauseCountdown) {
		gVoxPauseCountdown--;
	}
	if (gCurrentFunction == FUNCTION_TRANSMIT) {
		if (gAlarmState == ALARM_STATE_TXALARM || gAlarmState == ALARM_STATE_ALARM) {
			uint16_t Tone;

			gAlarmRunningCounter++;
			gAlarmToneCounter++;

			Tone = 500 + (gAlarmToneCounter * 25);
			if (Tone > 1500) {
				Tone = 500;
				gAlarmToneCounter = 0;
			}
			BK4819_SetScrambleFrequencyControlWord(Tone);
			if (gEeprom.ALARM_MODE == ALARM_MODE_TONE && gAlarmRunningCounter == 512) {
				gAlarmRunningCounter = 0;
				if (gAlarmState == ALARM_STATE_TXALARM) {
					gAlarmState = ALARM_STATE_ALARM;
					RADIO_EnableCxCSS();
					BK4819_SetupPowerAmplifier(0, 0);
					BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1, false);
					BK4819_Enable_AfDac_DiscMode_TxDsp();
					BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_RED, false);
					GUI_DisplayScreen();
				} else {
					gAlarmState = ALARM_STATE_TXALARM;
					GUI_DisplayScreen();
					BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_RED, true);
					RADIO_SetTxParameters();
					BK4819_TransmitTone(true, 500);
					SYSTEM_DelayMs(2);
					GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);
					gEnableSpeaker = true;
					gAlarmToneCounter = 0;
				}
			}
		}
		if (gRTTECountdown) {
			gRTTECountdown--;
			if (gRTTECountdown == 0) {
				FUNCTION_Select(FUNCTION_0);
				gUpdateDisplay = true;
			}
		}
	}
	if (gFmRadioMode && g_2000038E) {
		g_2000038E--;
		if (g_2000038E == 0) {
			FM_Start();
			GUI_SelectNextDisplay(DISPLAY_FM);
		}
	}
	if (gScreenToDisplay == DISPLAY_SCANNER) {
		uint32_t Result;
		int32_t Delta;
		BK4819_CssScanResult_t ScanResult;
		uint16_t CtcssFreq;

		if (g_2000045D) {
			g_2000045D--;
			APP_CheckKeys();
			return;
		}
		if (gScannerEditState != 0) {
			APP_CheckKeys();
			return;
		}

		switch (gScanCssState) {
		case SCAN_CSS_STATE_OFF:
			if (!BK4819_GetFrequencyScanResult(&Result)) {
				break;
			}

			Delta = Result - gScanFrequency;
			gScanFrequency = Result;
			if (Delta < 0) {
				Delta = -Delta;
			}
			if (Delta < 100) {
				gScanHitCount++;
			} else {
				gScanHitCount = 0;
			}
			BK4819_DisableFrequencyScan();
			if (gScanHitCount < 3) {
				BK4819_EnableFrequencyScan();
			} else {
				BK4819_SetScanFrequency(gScanFrequency);
				gScanCssResultIndex = 0xFF;
				gScanCssResultType = 0xFF;
				gScanHitCount = 0;
				gScanUseCssResult = false;
				gScanProgressIndicator = 0;
				gScanCssState = SCAN_CSS_STATE_SCANNING;
				GUI_SelectNextDisplay(DISPLAY_SCANNER);
			}
			g_2000045D = 21;
			break;

		case SCAN_CSS_STATE_SCANNING:
			ScanResult = BK4819_GetCxCSSScanResult(&Result, &CtcssFreq);
			if (ScanResult == BK4819_CSS_RESULT_NOT_FOUND) {
				break;
			}
			BK4819_Disable();
			if (ScanResult == BK4819_CSS_RESULT_CDCSS) {
				uint8_t Index;

				Index = DCS_GetCdcssIndex(Result);
				if (Index != 0xFF) {
					gScanCssResultIndex = Index;
					gScanCssResultType = CODE_TYPE_DIGITAL;
					gScanCssState = SCAN_CSS_STATE_FOUND;
					gScanUseCssResult = true;
				}
			} else if (ScanResult == BK4819_CSS_RESULT_CTCSS) {
				uint8_t Index;

				Index = DCS_GetCtcssIndex(CtcssFreq);
				if (Index != 0xFF) {
					if (Index == gScanCssResultIndex && gScanCssResultType == CODE_TYPE_CONTINUOUS_TONE) {
						gScanHitCount++;
						if (gScanHitCount >= 2) {
							gScanCssState = SCAN_CSS_STATE_FOUND;
							gScanUseCssResult = true;
						}
					} else {
						gScanHitCount = 0;
					}
					gScanCssResultType = CODE_TYPE_CONTINUOUS_TONE;
					gScanCssResultIndex = Index;
				}
			}
			if (gScanCssState < SCAN_CSS_STATE_FOUND) {
				BK4819_SetScanFrequency(gScanFrequency);
				g_2000045D = 21;
				break;
			}
			GUI_SelectNextDisplay(DISPLAY_SCANNER);
			break;
		default:
			break;
		}
	}

	if (gScreenToDisplay == DISPLAY_AIRCOPY && gAircopyState == AIRCOPY_TRANSFER && gAirCopyIsSendMode == 1) {
		if (gAircopySendCountdown) {
			gAircopySendCountdown--;
			if (gAircopySendCountdown == 0) {
				AIRCOPY_SendMessage();
				GUI_DisplayScreen();
			}
		}
	}

	APP_CheckKeys();
}

void APP_TimeSlice500ms(void)
{
	// Skipped authentic device check

	if (gKeypadLocked) {
		gKeypadLocked--;
		if (!gKeypadLocked) {
			gUpdateDisplay = true;
		}
	}

	// Skipped authentic device check

	if (gFmRadioCountdown) {
		gFmRadioCountdown--;
		return;
	}
	if (gReducedService) {
		BOARD_ADC_GetBatteryInfo(&gBatteryCurrentVoltage, &gBatteryCurrent);
		if (gBatteryCurrent > 500 || gBatteryCalibration[3] < gBatteryCurrentVoltage) {
			overlay_FLASH_RebootToBootloader();
		}
		return;
	}

	gBatteryCheckCounter++;

	// Skipped authentic device check

	if (gCurrentFunction != FUNCTION_TRANSMIT) {
		if ((gBatteryCheckCounter & 1) == 0) {
			BOARD_ADC_GetBatteryInfo(&gBatteryVoltages[gBatteryVoltageIndex++], &gBatteryCurrent);
			if (gBatteryVoltageIndex > 3) {
				gBatteryVoltageIndex = 0;
			}
			BATTERY_GetReadings(true);
		}
		if (gCurrentFunction != FUNCTION_POWER_SAVE) {
			gCurrentRSSI = BK4819_GetRSSI();
			UI_UpdateRSSI(gCurrentRSSI);
			if (gCurrentFunction == FUNCTION_TRANSMIT) {
				goto LAB_00004b08;
			}
		}
		if ((gFM_Step == 0 || gAskToSave) && gStepDirection == 0 && gCssScanMode == CSS_SCAN_MODE_OFF) {
			if (gBacklightCountdown) {
				gBacklightCountdown--;
				if (gBacklightCountdown == 0) {
					GPIO_ClearBit(&GPIOB->DATA, GPIOB_PIN_BACKLIGHT);
				}
			}
			if (gScreenToDisplay != DISPLAY_AIRCOPY && (gScreenToDisplay != DISPLAY_SCANNER || (gScanCssState >= SCAN_CSS_STATE_FOUND))) {
				if (gEeprom.AUTO_KEYPAD_LOCK && gKeyLockCountdown && !gDTMF_InputMode) {
					gKeyLockCountdown--;
					if (gKeyLockCountdown == 0) {
						gEeprom.KEY_LOCK = true;
					}
					gUpdateStatus = true;
				}
				if (g_20000393) {
					g_20000393--;
					if (g_20000393 == 0) {
						if (gInputBoxIndex || gDTMF_InputMode || gScreenToDisplay == DISPLAY_MENU) {
							AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
						}
						if (gScreenToDisplay == DISPLAY_SCANNER) {
							BK4819_StopScan();
							RADIO_ConfigureChannel(0, 2);
							RADIO_ConfigureChannel(1, 2);
							RADIO_SetupRegisters(true);
						}
						gWasFKeyPressed = false;
						gUpdateStatus = true;
						gInputBoxIndex = 0;
						gDTMF_InputMode = false;
						gDTMF_InputIndex = 0;
						gAskToSave = false;
						gAskToDelete = false;
						if (gFmRadioMode && gCurrentFunction != FUNCTION_RECEIVE && gCurrentFunction != FUNCTION_MONITOR && gCurrentFunction != FUNCTION_TRANSMIT) {
							GUI_SelectNextDisplay(DISPLAY_FM);
						} else {
							GUI_SelectNextDisplay(DISPLAY_MAIN);
						}
					}
				}
			}
		}
	}

LAB_00004b08:
	if (!gPttIsPressed && gFM_ResumeCountdown) {
		gFM_ResumeCountdown--;
		if (gFM_ResumeCountdown == 0) {
			RADIO_SetVfoState(VFO_STATE_NORMAL);
			if (gCurrentFunction != FUNCTION_RECEIVE && gCurrentFunction != FUNCTION_TRANSMIT && gCurrentFunction != FUNCTION_MONITOR && gFmRadioMode) {
				FM_Start();
				GUI_SelectNextDisplay(DISPLAY_FM);
			}
		}
	}

	if (gLowBattery) {
		gLowBatteryBlink = ++gLowBatteryCountdown & 1;
		UI_DisplayBattery(gLowBatteryCountdown);
		if (gCurrentFunction != FUNCTION_TRANSMIT) {
			if (gLowBatteryCountdown < 30) {
				if (gLowBatteryCountdown == 29 && !gChargingWithTypeC) {
					AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP);
				}
			} else {
				gLowBatteryCountdown = 0;
				if (!gChargingWithTypeC) {
					AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP);
					AUDIO_SetVoiceID(0, VOICE_ID_LOW_VOLTAGE);
					if (gBatteryDisplayLevel == 0) {
						AUDIO_PlaySingleVoice(true);
						gReducedService = true;
						FUNCTION_Select(FUNCTION_POWER_SAVE);
						ST7565_Configure_GPIO_B11();
						GPIO_ClearBit(&GPIOB->DATA, GPIOB_PIN_BACKLIGHT);
					} else {
						AUDIO_PlaySingleVoice(false);
					}
				}
			}
		}
	}

	if (gScreenToDisplay == DISPLAY_SCANNER && gScannerEditState == 0 && gScanCssState < SCAN_CSS_STATE_FOUND) {
		gScanProgressIndicator++;
		if (gScanProgressIndicator > 32) {
			if (gScanCssState == SCAN_CSS_STATE_SCANNING && !gScanSingleFrequency) {
				gScanCssState = SCAN_CSS_STATE_FOUND;
			} else {
				gScanCssState = SCAN_CSS_STATE_FAILED;
			}
		}
		gUpdateDisplay = true;
	}

	if (gDTMF_CallState != DTMF_CALL_STATE_NONE && gCurrentFunction != FUNCTION_TRANSMIT && gCurrentFunction != FUNCTION_RECEIVE) {
		if (gDTMF_AUTO_RESET_TIME) {
			gDTMF_AUTO_RESET_TIME--;
			if (gDTMF_AUTO_RESET_TIME == 0) {
				gDTMF_CallState = DTMF_CALL_STATE_NONE;
				gUpdateDisplay = true;
			}
		}
		if (gDTMF_DecodeRing && gDTMF_DecodeRingCountdown) {
			gDTMF_DecodeRingCountdown--;
			if ((gDTMF_DecodeRingCountdown % 3) == 0) {
				AUDIO_PlayBeep(BEEP_440HZ_500MS);
			}
			if (gDTMF_DecodeRingCountdown == 0) {
				gDTMF_DecodeRing = false;
			}
		}
	}

	if (gDTMF_IsTx && gDTMF_TxStopCountdown) {
		gDTMF_TxStopCountdown--;
		if (gDTMF_TxStopCountdown == 0) {
			gDTMF_IsTx = false;
			gUpdateDisplay = true;
		}
	}

	if (gDTMF_RecvTimeout) {
		gDTMF_RecvTimeout--;
		if (gDTMF_RecvTimeout == 0) {
			gDTMF_WriteIndex = 0;
			memset(gDTMF_Received, 0, sizeof(gDTMF_Received));
		}
	}
}

void FUN_00001150(void)
{
	gAlarmState = ALARM_STATE_OFF;
	GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);
	gEnableSpeaker = false;
	if (gEeprom.ALARM_MODE == ALARM_MODE_TONE) {
		RADIO_SendEndOfTransmission();
		RADIO_EnableCxCSS();
	}
	g_200003B6 = 0x50;
	SYSTEM_DelayMs(5);
	RADIO_SetupRegisters(true);
	gRequestDisplayScreen = DISPLAY_MAIN;
}

void FUN_000075b0(void)
{
	uint8_t StepSetting;
	uint16_t StepFrequency;

	BK4819_StopScan();
	RADIO_SelectVfos();

	if (IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE)) {
		gRxVfo->CHANNEL_SAVE = FREQ_CHANNEL_FIRST + 5;
	}
	StepSetting = gRxVfo->STEP_SETTING;
	StepFrequency = gRxVfo->StepFrequency;
	RADIO_InitInfo(gRxVfo, gRxVfo->CHANNEL_SAVE, gRxVfo->Band, gRxVfo->pCurrent->Frequency);

	gRxVfo->STEP_SETTING = StepSetting;
	gRxVfo->StepFrequency = StepFrequency;
	RADIO_SetupRegisters(true);

	gIsNoaaMode = false;
	if (gScanSingleFrequency) {
		gScanCssState = SCAN_CSS_STATE_SCANNING;
		gScanFrequency = gRxVfo->pCurrent->Frequency;
		gStepSetting = gRxVfo->STEP_SETTING;
		BK4819_PickRXFilterPathBasedOnFrequency(gScanFrequency);
		BK4819_SetScanFrequency(gScanFrequency);
	} else {
		gScanCssState = SCAN_CSS_STATE_OFF;
		gScanFrequency = 0xFFFFFFFF;
		BK4819_PickRXFilterPathBasedOnFrequency(0xFFFFFFFF);
		BK4819_EnableFrequencyScan();
	}
	g_2000045D = 21;
	gScanCssResultIndex = 0xFF;
	gScanCssResultType = 0xFF;
	gScanHitCount = 0;
	gScanUseCssResult = false;
	gDTMF_RequestPending = false;
	g_CxCSS_TAIL_Found = false;
	g_CDCSS_Lost = false;
	gCDCSSCodeType = 0;
	g_CTCSS_Lost = false;
	g_VOX_Lost = false;
	g_SquelchLost = false;
	gScannerEditState = 0;
	gScanProgressIndicator = 0;
}

void APP_SetStepDirection(bool bFlag, int8_t Direction)
{
	RADIO_SelectVfos();
	gNextMrChannel = gRxVfo->CHANNEL_SAVE;
	gCurrentScanList = 0;
	gStepDirection = Direction;
	if (IS_MR_CHANNEL(gNextMrChannel)) {
		if (bFlag) {
			gRestoreMrChannel = gNextMrChannel;
		}
		FUN_00007dd4();
	} else {
		if (bFlag) {
			gRestoreFrequency = gRxVfo->ConfigRX.Frequency;
		}
		APP_MoreRadioStuff();
	}
	ScanPauseDelayIn10msec = 50;
	gScheduleScanListen = false;
	gRxReceptionMode = RX_MODE_NONE;
	gScanPauseMode = false;
	g_20000413 = false;
}

static void APP_ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	bool bFlag;

	if (gCurrentFunction == FUNCTION_POWER_SAVE) {
		FUNCTION_Select(FUNCTION_0);
	}
	gBatterySaveCountdown = 1000;
	if (gEeprom.AUTO_KEYPAD_LOCK) {
		gKeyLockCountdown = 30;
	}
	if (!bKeyPressed) {
		if (gFlagSaveVfo) {
			SETTINGS_SaveVfoIndices();
			gFlagSaveVfo = false;
		}
		if (gFlagSaveSettings) {
			SETTINGS_SaveSettings();
			gFlagSaveSettings = false;
		}
		if (gFlagSaveFM) {
			SETTINGS_SaveFM();
			gFlagSaveFM = false;
		}
		if (gFlagSaveChannel) {
			SETTINGS_SaveChannel(
				gTxVfo->CHANNEL_SAVE,
				gEeprom.TX_CHANNEL,
				gTxVfo,
				gFlagSaveChannel);
			gFlagSaveChannel = false;
			RADIO_ConfigureChannel(gEeprom.TX_CHANNEL, 1);
			RADIO_SetupRegisters(true);
			GUI_SelectNextDisplay(DISPLAY_MAIN);
		}
	} else {
		if (Key != KEY_PTT) {
			g_20000393 = 0x10;
		}
		BACKLIGHT_TurnOn();
		if (gDTMF_DecodeRing) {
			gDTMF_DecodeRing = false;
			AUDIO_PlayBeep(BEEP_1KHZ_60MS_OPTIONAL);
			if (Key != KEY_PTT) {
				gPttWasReleased = true;
				return;
			}
		}
	}

	if (gEeprom.KEY_LOCK && gCurrentFunction != FUNCTION_TRANSMIT && Key != KEY_PTT) {
		if (Key == KEY_F) {
			if (!bKeyHeld) {
				if (!bKeyPressed) {
					return;
				}
				if (bKeyHeld) {
					return;
				}
				AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
				gKeypadLocked = 4;
				gUpdateDisplay = true;
				return;
			}
			if (!bKeyPressed) {
				return;
			}
		} else if (Key != KEY_SIDE1 && Key != KEY_SIDE2) {
			if (!bKeyPressed) {
				return;
			}
			if (bKeyHeld) {
				return;
			}
			AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
			gKeypadLocked = 4;
			gUpdateDisplay = true;
			return;
		}
	}

	if ((gStepDirection && Key != KEY_PTT && Key != KEY_UP && Key != KEY_DOWN && Key != KEY_EXIT && Key != KEY_STAR) ||
	    (gCssScanMode != CSS_SCAN_MODE_OFF && Key != KEY_PTT && Key != KEY_UP && Key != KEY_DOWN && Key != KEY_EXIT && Key != KEY_STAR && Key != KEY_MENU)) {
		if (!bKeyPressed || bKeyHeld) {
			return;
		}
		AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
		return;
	}

	bFlag = false;

	if (gPttWasPressed && Key == KEY_PTT) {
		bFlag = bKeyHeld;
		if (!bKeyPressed) {
			bFlag = true;
			gPttWasPressed = false;
		}
	}

	if (gPttWasReleased && Key != KEY_PTT) {
		if (bKeyHeld) {
			bFlag = true;
		}
		if (!bKeyPressed) {
			bFlag = true;
			gPttWasReleased = false;
		}
	}

	if (gWasFKeyPressed && KEY_9 < Key && Key != KEY_F && Key != KEY_STAR) {
		gWasFKeyPressed = false;
		gUpdateStatus = true;
	}

	if (gF_LOCK) {
		if (Key == KEY_PTT) {
			return;
		}
		if (Key == KEY_SIDE2) {
			return;
		}
		if (Key == KEY_SIDE1) {
			return;
		}
	}

	if (!bFlag) {
		if (gCurrentFunction == FUNCTION_TRANSMIT) {
			if (gAlarmState == ALARM_STATE_OFF) {
				if (Key == KEY_PTT) {
					GENERIC_Key_PTT(bKeyPressed);
				} else {
					char Code;

					if (Key == KEY_SIDE2) {
						Code = 0xFE;
					} else {
						Code = DTMF_GetCharacter(Key);
						if (Code == 0xFF) {
							goto Skip;
						}
					}

					if (bKeyHeld || !bKeyPressed) {
						if (!bKeyPressed) {
							GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);
							gEnableSpeaker = false;
							BK4819_ExitDTMF_TX(false);
							if (gCurrentVfo->SCRAMBLING_TYPE == 0 || !gSetting_ScrambleEnable) {
								BK4819_DisableScramble();
							} else {
								BK4819_EnableScramble(gCurrentVfo->SCRAMBLING_TYPE - 1);
							}
						}
					} else {
						if (gEeprom.DTMF_SIDE_TONE) {
							GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);
							gEnableSpeaker = true;
						}
						BK4819_DisableScramble();
						if (Code == 0xFE) {
							BK4819_TransmitTone(gEeprom.DTMF_SIDE_TONE, 1750);
						} else {
							BK4819_PlayDTMFEx(gEeprom.DTMF_SIDE_TONE, Code);
						}
					}
				}
			} else if (!bKeyHeld && bKeyPressed) {
				FUN_00001150();
				if (gEeprom.REPEATER_TAIL_TONE_ELIMINATION == 0) {
					FUNCTION_Select(FUNCTION_0);
				} else {
					gRTTECountdown = gEeprom.REPEATER_TAIL_TONE_ELIMINATION * 10;
				}
				if (Key == KEY_PTT) {
					gPttWasPressed = true;
				} else {
					gPttWasReleased = true;
				}
			}
		} else if (Key != KEY_SIDE1 && Key != KEY_SIDE2) {
			switch (gScreenToDisplay) {
			case DISPLAY_MAIN:
				MAIN_ProcessKeys(Key, bKeyPressed, bKeyHeld);
				break;
			case DISPLAY_FM:
				FM_ProcessKeys(Key, bKeyPressed, bKeyHeld);
				break;
			case DISPLAY_MENU:
				MENU_ProcessKeys(Key, bKeyPressed, bKeyHeld);
				break;
			case DISPLAY_SCANNER:
				SCANNER_ProcessKeys(Key, bKeyPressed, bKeyHeld);
				break;
			case DISPLAY_AIRCOPY:
				AIRCOPY_ProcessKeys(Key, bKeyPressed, bKeyHeld);
				break;
			default:
				break;
			}
		} else if (gScreenToDisplay != DISPLAY_SCANNER && gScreenToDisplay != DISPLAY_AIRCOPY) {
			ACTION_Handle(Key, bKeyPressed, bKeyHeld);
		} else if (!bKeyHeld && bKeyPressed) {
			gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
		}
	}

Skip:
	if (gBeepToPlay) {
		AUDIO_PlayBeep(gBeepToPlay);
		gBeepToPlay = BEEP_NONE;
	}

	if (gFlagAcceptSetting) {
		MENU_AcceptSetting();
		gFlagRefreshSetting = true;
		gFlagAcceptSetting = false;
	}
	if (gFlagStopScan) {
		BK4819_StopScan();
		gFlagStopScan = false;
	}
	if (gRequestSaveSettings) {
		if (bKeyHeld == 0) {
			SETTINGS_SaveSettings();
		} else {
			gFlagSaveSettings = 1;
		}
		gRequestSaveSettings = false;
		gUpdateStatus = true;
	}
	if (gRequestSaveFM) {
		if (!bKeyHeld) {
			SETTINGS_SaveFM();
		} else {
			gFlagSaveFM = true;
		}
		gRequestSaveFM = false;
	}
	if (gRequestSaveVFO) {
		if (!bKeyHeld) {
			SETTINGS_SaveVfoIndices();
		} else {
			gFlagSaveVfo = true;
		}
		gRequestSaveVFO = false;
	}
	if (gRequestSaveChannel) {
		if (!bKeyHeld) {
			SETTINGS_SaveChannel(gTxVfo->CHANNEL_SAVE, gEeprom.TX_CHANNEL, gTxVfo, gRequestSaveChannel);
			if (gScreenToDisplay != DISPLAY_SCANNER) {
				gVfoConfigureMode = VFO_CONFIGURE_1;
			}
		} else {
			gFlagSaveChannel = gRequestSaveChannel;
			if (gRequestDisplayScreen == DISPLAY_INVALID) {
				gRequestDisplayScreen = DISPLAY_MAIN;
			}
		}
		gRequestSaveChannel = 0;
	}


	if (gVfoConfigureMode != VFO_CONFIGURE_0) {
		if (gFlagResetVfos) {
			RADIO_ConfigureChannel(0, gVfoConfigureMode);
			RADIO_ConfigureChannel(1, gVfoConfigureMode);
		} else {
			RADIO_ConfigureChannel(gEeprom.TX_CHANNEL, gVfoConfigureMode);
		}
		if (gRequestDisplayScreen == DISPLAY_INVALID) {
			gRequestDisplayScreen = DISPLAY_MAIN;
		}
		gFlagReconfigureVfos = true;
		gVfoConfigureMode = VFO_CONFIGURE_0;
		gFlagResetVfos = false;
	}

	if (gFlagReconfigureVfos) {
		RADIO_SelectVfos();
		RADIO_ConfigureNOAA();
		RADIO_SetupRegisters(true);
		gDTMF_AUTO_RESET_TIME = 0;
		gDTMF_CallState = DTMF_CALL_STATE_NONE;
		gDTMF_TxStopCountdown = 0;
		gDTMF_IsTx = false;
		gVFO_RSSI_Level[0] = 0;
		gVFO_RSSI_Level[1] = 0;
		gFlagReconfigureVfos = false;
	}

	if (gFlagRefreshSetting) {
		MENU_ShowCurrentSetting();
		gFlagRefreshSetting = false;
	}
	if (gFlagStartScan) {
		AUDIO_SetVoiceID(0, VOICE_ID_SCANNING_BEGIN);
		AUDIO_PlaySingleVoice(true);
		FUN_000075b0();
		gRequestDisplayScreen = DISPLAY_SCANNER;
		gFlagStartScan = false;
	}
	if (g_200003A0 == 1) {
		RADIO_PrepareTX();
		g_200003A0 = 0;
	}
	if (gAnotherVoiceID != VOICE_ID_INVALID) {
		if (gAnotherVoiceID < 76) {
			AUDIO_SetVoiceID(0, gAnotherVoiceID);
		}
		AUDIO_PlaySingleVoice(false);
		gAnotherVoiceID = VOICE_ID_INVALID;
	}
	GUI_SelectNextDisplay(gRequestDisplayScreen);
	gRequestDisplayScreen = DISPLAY_INVALID;
}

