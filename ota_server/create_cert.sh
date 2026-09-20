#!/bin/env bash

echo "Gerando certificado..."
echo "Digite o ip do servidor:"
read ip

# Gera chave privada e certificado autoassinado (válido por 365 dias)
openssl req -x509 -newkey rsa:2048 -keyout server_key.pem -out server_cert.pem \
-days 365 -nodes -subj "/CN=$ip"

echo "Arquivos criados com sucesso!"
echo "Certificado gerado em server_cert.pem"
echo "Chave privada em server_key.pem"

# O certificado já está em server_cert.pem, mas vamos garantir que está no formato correto
openssl x509 -in server_cert.pem -out esp32_cert.pem -outform PEM

xxd -i esp32_cert.pem ../components/ota/include/server_cert.h

exit 0
