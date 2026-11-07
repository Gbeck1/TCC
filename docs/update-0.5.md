# Atualização 0.5.0 por SSH local

Esta versão adiciona associação provisória dos DS18B20, troca posterior de sensores e acesso exclusivo à USB, mantendo o teclado numérico e o Peltier Liga/Desliga. Se há avisos de subtensão, estabilize a alimentação da Pi antes de gravar firmware ou configurações; veja [usb-power.md](usb-power.md). A instalação existente informada pelo usuário está em `/home/beck/Desktop/TCC-NB3`. A atualização preserva usuários/histórico na Pi e o formato EEPROM v2 no Mega. Feche o programa antes de copiar os arquivos.

## Transferir o pacote

No Windows, baixe `tcc-nb3-0.5.0.zip` e salve em Downloads com esse nome. No PowerShell do Windows, envie o arquivo com o mesmo nome local utilizado para entrar por SSH. Substitua `NOME_DA_RASPBERRY` pelo resultado do comando `hostname` na Pi:

```powershell
scp "$env:USERPROFILE\Downloads\tcc-nb3-0.5.0.zip" beck@NOME_DA_RASPBERRY.local:/tmp/tcc-nb3-0.5.0.zip
```

## Atualizar na sessão SSH

Cole o bloco completo dentro da sessão SSH, como usuário beck:

```bash
bash <<'TCC_UPDATE'
set -euo pipefail
tcc_app_dir="/home/beck/Desktop/TCC-NB3"
tcc_zip="/tmp/tcc-nb3-0.5.0.zip"
test -d "$tcc_app_dir/raspberry/tcc_monitor"
test -f "$tcc_zip"
tcc_temp_dir=$(mktemp -d)
python3 -m zipfile -e "$tcc_zip" "$tcc_temp_dir"
tcc_source="$tcc_temp_dir/TCC-NB3"
(cd "$tcc_source" && sha256sum --check SHA256SUMS)
cd "$tcc_app_dir"
tcc_python=python3
if [[ -x .venv/bin/python ]]; then tcc_python=.venv/bin/python; fi
PYTHONPATH="$tcc_source/raspberry" "$tcc_python" -c \
  'import tkinter, serial; from tcc_monitor.ui import MonitorApp'
tcc_backup=$(mktemp -d "$tcc_app_dir/backup-0.5-XXXXXX")
cp -a raspberry "$tcc_backup/"
if [[ -d firmware ]]; then cp -a firmware "$tcc_backup/"; fi
mkdir -p firmware
cp -a "$tcc_source/raspberry/." raspberry/
cp -a "$tcc_source/firmware/." firmware/
printf '\nAplicação e arquivos do firmware atualizados. Backup: %s\n' "$tcc_backup"
printf 'O Arduino ainda precisa receber o novo firmware para associar os sensores automaticamente.\n'
TCC_UPDATE
```

O bloco não inicia a aplicação e não grava o Arduino automaticamente. A `.venv` e o banco de dados permanecem no local. Antes de extrair, o ZIP também pode ser conferido pelo SHA256SUMS publicado junto aos pacotes no GitHub.

## Gravar o Mega

Pelo Arduino IDE, abra o novo `firmware/mega_controller/mega_controller.ino`, selecione Mega 2560/ATmega2560 e use Verificar e Carregar. As bibliotecas continuam ArduinoJson 6.21.5, OneWire 2.3.8 e DallasTemperature 4.0.6.

Ou, na Pi, com a aplicação fechada e o Mega conectado por USB:

```bash
cd /home/beck/Desktop/TCC-NB3
ls -l /dev/serial/by-id/
bash scripts/upload_mega.sh /dev/serial/by-id/IDENTIFICADOR_REAL_DO_MEGA
```

Use o identificador real listado. O script grava `firmware/compiled/mega_controller.hex` e verifica a escrita. O pacote novo contém esse HEX já compilado; o formato EEPROM v2 permanece o mesmo e as saídas manuais não são restauradas após reset, como antes.

Depois abra pelo ícone ou pela sessão SSH na tela gráfica local:

```bash
cd /home/beck/Desktop/TCC-NB3
nohup env DISPLAY=:0 bash scripts/run.sh --fullscreen > "$HOME/tcc-monitor.log" 2>&1 < /dev/null &
```

Essa abertura considera uma sessão gráfica local em :0. Para conferir erro de inicialização, leia `tail -n 40 "$HOME/tcc-monitor.log"`. Mantenha uma única instância da aplicação aberta.

## Conferir os sensores

Com os seis DS18B20 conectados ao D36 antes da partida, o firmware associa cada ROM a uma sala ainda sem sensor e grava na EEPROM. As salas já configuradas são preservadas. Em Manutenção → Sensores, confirme Sensores encontrados: 6 e seis associações únicas. Depois corrija a localização escolhendo sala e sensor; a opção troca os dois vínculos e precisa de Gravar configuração no Arduino para persistir. Um sensor desconectado não é substituído silenciosamente. Menos de seis dispositivos detectados exige conferir o barramento, sem fabricar medidas.

## Conferir os demais controles

Entre como ADMIN e selecione Manual. Toque num PWM de ventilador, digite o valor e escolha Concluir; confira que ele só é enviado ao usar Aplicar saídas. Cancele uma edição para confirmar que o valor anterior foi preservado. Marque/desmarque Ligar no Peltier e use Aplicar saídas; confira Ligado/Desligado na telemetria. No Automático, confira a média dos seis sensores e os pontos de atuação: liga acima de SP + 0,5°C, desliga ao chegar em SP e mantém o estado entre os dois valores. Sensores inválidos preservam a última saída, sem acrescentar desligamento ou intertravamento.
