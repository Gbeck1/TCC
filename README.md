# TCC NB3 — pacote de instalação

Pacote preparado para Raspberry Pi 3B com Raspberry Pi OS 64 bits e Arduino Mega 2560.

Baixe `pacotes/tcc-nb3-raspberry.zip` e copie pelo pendrive. Extraia a pasta TCC-NB3 para a pasta pessoal da Pi. Como usuário normal, execute:

```bash
cd ~/TCC-NB3
bash scripts/install_raspberry.sh
```

O instalador prepara a interface e o ícone de desktop, sem autostart. Pacotes de sistema podem precisar de internet na primeira instalação; a dependência Python é incluída para instalação offline.

O firmware compilado está em `firmware/mega_controller.hex` e também dentro do pacote. A gravação no Arduino é uma etapa explícita e separada. Os códigos-fonte, testes e instruções completas estão dentro do ZIP, em TCC-NB3.

Confira os hashes em SHA256SUMS. O pacote foi verificado por integridade e o instalador testado numa cópia isolada; nenhuma placa física foi gravada pela nuvem.
