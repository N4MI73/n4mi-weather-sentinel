# Weather Sentinel — backend server container.
#
# Deliberately minimal: one Python file, two dependencies (Flask,
# requests), no build step beyond pip install. Matches the same shape
# as propmon's own Dockerfile (the closest existing NAS precedent for
# this exact Tempest+NWS combination).

FROM python:3.12-slim

WORKDIR /app

COPY weather_sentinel_server.py .

RUN pip install --no-cache-dir flask requests

CMD ["python", "weather_sentinel_server.py"]
