# Host-тесты Domain

Тесты собираются на хосте (MSVC + Ninja), без ESP-IDF. `cmake` нет в PATH — конфигурация
и сборка идут из окружения Visual Studio.

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
set CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
"%CMAKE%" -S esp32-p4_HA_v2\domain\tests -B esp32-p4_HA_v2\domain\tests\build -G Ninja -DCMAKE_BUILD_TYPE=Debug
"%CMAKE%" --build esp32-p4_HA_v2\domain\tests\build
"%CMAKE%" --build esp32-p4_HA_v2\domain\tests\build --target test
```

Пути — от корня репозитория (`ESP32_ZB_Automatisation`). `vcvarsall.bat` задаёт и
компилятор, и `ninja` (лежит в `Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja`);
без него CMake не находит `cl.exe`.

`--target test` — это CTest через CMake, отдельный бинарник `ctest` в PATH не нужен.

Переконфигурация после правки `CMakeLists.txt` — той же командой `-S/-B`. Если кэш
испорчен (например, остался `CMAKE_MAKE_PROGRAM` на несуществующий путь) — удалить
каталог `build` и повторить.

Тесты собираются с `/W4`; предупреждения недопустимы.
