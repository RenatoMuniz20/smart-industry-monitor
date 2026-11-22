# InterSCity falso: só biblioteca padrão do Python, sem dependências.
FROM python:3.12-alpine
RUN addgroup -g 10001 fake \
    && adduser -D -H -u 10001 -G fake fake \
    && mkdir /data && chown 10001:10001 /data
COPY fake_interscity.py /app/fake_interscity.py
USER 10001:10001
EXPOSE 8000
STOPSIGNAL SIGTERM
ENTRYPOINT ["python3", "-I", "/app/fake_interscity.py"]
