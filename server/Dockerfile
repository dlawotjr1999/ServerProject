# ---- build stage ----
# 컴파일에 필요한 gcc 툴체인은 이 스테이지에만 있으면 되고, 최종 런타임 이미지에는 남기지 않음
FROM gcc:13 AS build

# hiredis 헤더/라이브러리 (3단계) - libhiredis-dev는 런타임 .so도 의존성으로 함께 설치함
RUN apt-get update && apt-get install -y --no-install-recommends libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# 소스 전체를 빌드 컨테이너 안으로 복사한 뒤, 기존 Makefile 그대로 사용해 빌드
# (호스트 OS/컴파일러 버전에 상관없이 컨테이너 안에서 재현 가능하게 만드는 것이 이 단계의 목적)
COPY . .
RUN make clean && make

# ---- runtime stage ----
# 빌드 도구가 전혀 필요 없는 최소 이미지. 공격 표면과 이미지 크기를 줄이기 위해 slim 사용
FROM debian:bookworm-slim

# hiredis 런타임 라이브러리 (3단계) - -dev 패키지를 그대로 써서 정확한 런타임 전용 패키지명을
# 추측하지 않음(포트폴리오 규모에서 이미지 크기보다 확실한 동작을 우선함)
RUN apt-get update && apt-get install -y --no-install-recommends libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

# 컨테이너 안에서 root로 프로세스를 띄우지 않도록 전용 사용자/그룹 생성
# 3800/9100은 1024 이상의 비특권 포트라 non-root로도 bind 가능함
RUN groupadd --system chat && useradd --system --gid chat --no-create-home chat

WORKDIR /app

# 빌드 스테이지에서 만든 실행 파일 하나만 가져옴(컴파일러/헤더/오브젝트 파일은 런타임 이미지에 남지 않음)
COPY --from=build /src/server /app/server

USER chat

# 채팅 서버 포트(3800)와 메트릭/헬스체크 포트(9100)
EXPOSE 3800 9100

# exec 형태로 실행해야 이 프로세스가 컨테이너의 PID 1이 되어 SIGTERM을 직접 받음
# (셸 형태로 실행하면 셸이 PID 1이 되어 SIGTERM이 서버 프로세스까지 전달되지 않고,
#  main.c의 graceful shutdown(0단계 #3)이 k8s pod 종료 시 동작하지 않게 됨)
ENTRYPOINT ["/app/server"]
