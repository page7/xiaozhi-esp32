# 缂栬瘧涓庣儳褰曟寚鍗楋紙鍛戒护琛屾ā寮忥級

鏈」鐩湪鏈満鐨?ESP-IDF 瀹夎浣跨敤浜?*闈為粯璁よ矾寰?*锛坄D:\Program\.espressif` / `D:\Program\esp\v6.1\esp-idf`锛夛紝骞朵笖绯荤粺 PATH 涓殑 `python` 鏄?pyenv 鐨?3.10锛堜笌 IDF 鐨?venv 3.11 涓嶇锛夈€傚洜姝ょ紪璇?鐑у綍鍓?*蹇呴』鍏堝姞杞界幆澧冭剼鏈?*锛屽惁鍒欎細鎶ワ細

```
ERROR: ESP-IDF Python virtual environment "C:\Users\zhoun\.espressif\python_env\idf6.1_py3.10_env\...python.exe" not found.
```

## 1. 鍔犺浇鐜锛堟瘡娆℃柊寮€缁堢閮借鍋氫竴娆★級

鍦ㄤ粨搴撴牴鐩綍锛坄D:\ai\xiaozhi-esp32`锛夌殑 PowerShell 涓細

```powershell
. .\.vscode\idf-env.ps1
```

楠岃瘉锛?

```powershell
idf.py --version        # 搴旇緭鍑?ESP-IDF v6.1
echo $env:IDF_PYTHON_ENV_PATH   # 搴旀寚鍚?D:\Program\.espressif\python_env\idf6.1_py3.11_env
```

> `idf-env.ps1` 鍋氫簡涓変欢浜嬶細璁剧疆 `IDF_TOOLS_PATH`銆佹妸 IDF 鑷甫 Python 3.11.2 鎻掑埌 PATH 鏈€鍓嶃€佽皟鐢?`export.ps1`銆傝嫢浠ュ悗 IDF 鎴?Python 璺緞鍙樺寲锛屽彧闇€鏀硅鏂囦欢銆?

## 2. 鏌ヨ鏉垮瓙涓庡彉浣?

```powershell
# 鍒楀嚭鍏ㄩ儴 board 鐩綍鍜?variant锛堢缉杩涚殑 "- xxx" 鏄彉浣撳悕锛?
python scripts/build.py --list-boards

# 鍏朵粬鏌ヨ
python scripts/build.py --list-languages      # 鏀寔鐨勫浐浠惰瑷€
python scripts/build.py --list-wake-words     # 鍙敤鍞ら啋璇嶆ā鍨?
```

鍙樹綋鍚嶉€夋嫨瑙勫垯锛?
- 鍒楄〃涓湁缂╄繘瀛愰」 鈫?`boardDir` 濉埗鍚嶏紝`variantName` 濉缉杩涘悕
- 娌℃湁缂╄繘瀛愰」 鈫?`variantName` 閫氬父涓恒€屽巶鍟?鍨嬪彿銆嶅叏鍚嶏紙濡?`waveshare-esp32-s3-touch-lcd-1.46` 瀵瑰簲鐩綍 `waveshare/esp32-s3-touch-lcd-1.46`锛?

## 3. 缂栬瘧

```powershell
python scripts/build.py <boardDir> --name <variantName>
```

甯哥敤鍙€夊弬鏁帮細

| 鍙傛暟 | 璇存槑 | 绀轰緥 |
|---|---|---|
| `--language` | 鍥轰欢璇█ | `--language zh-CN` |
| `--wake-word` | 鍞ら啋璇嶆ā鍨?/ `nihaoxiaozhi` / `disabled` | `--wake-word nihaoxiaozhi` |
| `--build-options-json` | 鏉跨骇璇箟閫夐」锛圝SON锛?| `--build-options-json "{...}"` |
| `--zip` | 棰濆鐢熸垚 `releases/v<鐗堟湰>_<name>.zip` | |

**瀹炰緥锛堟湰鏈哄凡楠岃瘉锛?*锛?

```powershell
python scripts/build.py waveshare/esp32-s3-touch-lcd-1.46 --name esp32-s3-touch-lcd-1.46
```

缂栬瘧鎴愬姛鍚庝骇鐗╁湪 `build\`锛?

- `xiaozhi.bin` 鈥?涓诲浐浠?
- `merged-binary.bin` 鈥?鏁寸墖鍚堝苟闀滃儚锛堟柊鏉?閲忎骇棣栭€夛紝浠?0x0 涓€娆＄儳瀹岋級
- `bootloader\bootloader.bin`銆乣partition_table\partition-table.bin`銆乣ota_data_initial.bin`銆乣generated_assets.bin` 鈥?鍒嗗尯闀滃儚

> 娉ㄦ剰锛歚build.py` 浼氭敼鍐欐湰鍦?`sdkconfig` 鍜?`build/` 鐩綍銆傚垏鎹笉鍚屾澘瀛愮紪璇戝睘姝ｅ父鐜拌薄锛屼笉瑕佹妸 `build/` 鎴?`sdkconfig` 鐨勫彉鍔ㄥ綋浣滄簮鐮佹敼鍔ㄦ彁浜ゃ€?

## 4. 鐑у綍涓庣洃瑙?

```powershell
# 鏌ョ湅 COM 鍙ｏ紙璁惧绠＄悊鍣ㄤ篃鍙級
[System.IO.Ports.SerialPort]::GetPortNames()

# 鐑у綍 + 鎵撳紑涓插彛鐩戣锛圕trl+] 閫€鍑虹洃瑙嗭級
idf.py -p COM7 flash monitor

# 鍏ㄦ柊鏉垮瓙寤鸿鍏堟暣鐗囨摝闄わ紙浼氭竻闄?NVS 閲屼繚瀛樼殑 WiFi 閰嶇疆锛?
idf.py -p COM7 erase-flash
idf.py -p COM7 flash monitor

# 鎴栬€呯洿鎺ョ儳鏁寸墖鍚堝苟闀滃儚锛堢瓑浠蜂簬涓婇潰 4 涓垎鍖虹殑鍦板潃琛級
python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset write-flash 0x0 build\merged-binary.bin

# 浠呯洃瑙嗘棩蹇楋紙涓嶇儳褰曪級
idf.py -p COM7 monitor
```

涓插彛鎵撲笉寮€鏃舵寜浣忔澘涓?**BOOT** 閿啀鎸?**RESET** 杩涘叆涓嬭浇妯″紡鍚庨噸璇曘€?

## 5. VS Code 浠诲姟锛堢瓑浠峰浘褰㈠寲鍏ュ彛锛?

`.vscode/tasks.json` 宸查厤缃悓鏍锋祦绋嬶紙姣忎釜浠诲姟鍐呴儴閮戒細鍏?source `idf-env.ps1`锛夛細

- **IDF: List Boards** 鈥?鍒楀嚭鏉垮瓙/鍙樹綋
- **IDF: Build (build.py)** 鈥?缂栬瘧锛堟彁绀鸿緭鍏?boardDir / variantName锛?
- **IDF: Flash Monitor** 鈥?鐑у綍骞剁洃瑙嗭紙鎻愮ず杈撳叆 COM 鍙ｏ級
- **IDF: Host Tests** 鈥?杩愯 `scripts/tests` 鍗曞厓娴嬭瘯

鍏ュ彛锛歚Ctrl+Shift+P` 鈫?`Tasks: Run Task`锛屾垨 `Ctrl+Shift+B` 鐩存帴缂栬瘧銆?

## 6. 甯歌闂

| 鐜拌薄 | 澶勭悊 |
|---|---|
| `idf6.1_py3.10_env not found` | 娌″姞杞?`idf-env.ps1`锛岃绗?1 鑺?|
| `ESP-IDF is not active; listing variants for ESP-IDF 6.0.2` 璀﹀憡 | 鍚屼笂锛岀幆澧冩湭婵€娲伙紙浠呭奖鍝嶅垪琛ㄦ彁绀猴紝缂栬瘧鍓嶅繀椤绘縺娲伙級 |
| 鎺у埗鍙颁腑鏂?Unicode 鍛婅 | `idf-env.ps1` 宸茶 `chcp 65001` 鍜?`PYTHONUTF8=1`锛涢噸寮€缁堢鍐嶈瘯 |
| 鐑у綍澶辫触 / 杩炰笉涓婁覆鍙?| 妫€鏌?COM 鍙凤紱鎸変綇 BOOT 杩涗笅杞芥ā寮忥紱鎹?USB 绾?鍙?|
| 缂栬瘧鏋佹參 | 姝ｅ父鐜拌薄锛圵indows + 棣栨涓嬭浇 managed_components锛夛紱浜屾澧為噺缂栬瘧蹇?|
| 鍒囨崲鏉垮瓙鍚庤涓哄紓甯?| `build.py` 宸茶嚜鍔ㄩ噸鏂伴厤缃?target锛屾棤闇€鎵嬪姩娓呯悊锛涘繀瑕佹椂鍒?`build/` 閲嶇紪 |

## 7. 鏈満鐜鍙傛暟閫熸煡

| 椤?| 鍊?|
|---|---|
| ESP-IDF | `D:\Program\esp\v6.1\esp-idf`锛坴6.1锛?|
| IDF_TOOLS_PATH | `D:\Program\.espressif` |
| Python venv | `D:\Program\.espressif\python_env\idf6.1_py3.11_env` |
| 鐜鑴氭湰 | `.vscode/idf-env.ps1` |
| VS Code 鎵╁睍閰嶇疆 | `%APPDATA%\Code\User\settings.json`锛坄idf.*` 閿級 |
