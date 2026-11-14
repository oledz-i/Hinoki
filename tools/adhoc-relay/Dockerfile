FROM python:3.12-alpine
WORKDIR /app
COPY relay.py .
RUN adduser -D -H relay
USER relay
ENV RELAY_PORT=27360
EXPOSE 27360/tcp
CMD ["python", "-u", "relay.py"]
