#!/bin/bash
# Capture MAVLink UDP traffic and train a Zstd dictionary for compressed
# (ZstdCompression) endpoints. See the "ZSTD compression" section of README.md.
#
# Usage:
#   ./train-zstd-dict.sh [port] [duration_seconds] [output_dict]
#   ./train-zstd-dict.sh --pcap <file.pcap> [port] [output_dict]
#   ./train-zstd-dict.sh --folder <dir> [port] [output_dict]
#
# Examples:
#   ./train-zstd-dict.sh 14550 120 mavlink_zstd.dict
#   ./train-zstd-dict.sh --pcap capture.pcap 0 mavlink_zstd.dict
#   ./train-zstd-dict.sh --folder /path/to/pcaps 0 mavlink_zstd.dict

set -e

# Configuration
INPUT_FOLDER=""
INPUT_PCAP=""

if [ "$1" = "--folder" ] || [ "$1" = "--dir" ] || [ "$1" = "-f" ]; then
    INPUT_FOLDER="$2"
    PORT=${3:-0}                                    # Auto-detect port if not specified
    OUTPUT_DICT=${4:-mavlink_zstd.dict}
    DURATION=0
elif [ "$1" = "--pcap" ] || [ "$1" = "-p" ]; then
    INPUT_PCAP="$2"
    PORT=${3:-0}                                    # Auto-detect port if not specified
    OUTPUT_DICT=${4:-mavlink_zstd.dict}
    DURATION=0
else
    PORT=${1:-14550}                                # Default MAVLink port
    DURATION=${2:-60}                               # Capture duration in seconds
    OUTPUT_DICT=${3:-mavlink_zstd.dict}             # Output dictionary filename
    INPUT_PCAP=${4:-""}                             # Optional existing pcap file
fi

CAPTURE_FILE="mavlink_capture_${PORT}.pcap"
PAYLOAD_FILE="mavlink_payloads_${PORT}.bin"
DICT_SIZE=${DICT_SIZE:-16384}                       # 16KB dictionary (can be set via env)

echo "=== MAVLink Traffic Capture & Zstd Dictionary Training ==="
if [ -n "$INPUT_FOLDER" ]; then
    echo "Input folder: $INPUT_FOLDER"
    if [ "$PORT" -eq 0 ]; then
        echo "Port: Auto-detect"
    else
        echo "Port: $PORT"
    fi
elif [ -n "$INPUT_PCAP" ]; then
    echo "Input PCAP: $INPUT_PCAP"
    if [ "$PORT" -eq 0 ]; then
        echo "Port: Auto-detect"
    else
        echo "Port: $PORT"
    fi
else
    echo "Port: $PORT"
    echo "Duration: ${DURATION}s"
fi
echo "Output dictionary: $OUTPUT_DICT"
echo ""

# Check dependencies
if [ -n "$INPUT_FOLDER" ] || [ -n "$INPUT_PCAP" ]; then
    for cmd in tshark zstd; do
        if ! command -v $cmd &> /dev/null; then
            echo "Error: $cmd is required but not installed"
            exit 1
        fi
    done
else
    for cmd in tcpdump tshark zstd; do
        if ! command -v $cmd &> /dev/null; then
            echo "Error: $cmd is required but not installed"
            exit 1
        fi
    done
fi

# Temporary directory for individual packet samples (zstd trains better on separate files)
SAMPLES_DIR=$(mktemp -d)

# Cleanup function
cleanup() {
    echo "Cleaning up temporary files..."
    if [ -z "$INPUT_PCAP" ]; then
        rm -f "$CAPTURE_FILE"
    fi
    rm -f "$PAYLOAD_FILE"
    rm -rf "$SAMPLES_DIR"
}
trap cleanup EXIT

# Step 1: Capture or use existing pcap(s)
PCAP_FILES=()  # Array to hold pcap files to process

if [ -n "$INPUT_FOLDER" ]; then
    if [ ! -d "$INPUT_FOLDER" ]; then
        echo "Error: Input folder '$INPUT_FOLDER' not found or is not a directory"
        exit 1
    fi
    
    # Find all pcap files in the folder
    while IFS= read -r -d '' file; do
        PCAP_FILES+=("$file")
    done < <(find "$INPUT_FOLDER" -maxdepth 1 -type f \( -name "*.pcap" -o -name "*.pcapng" \) -print0 | sort -z)
    
    if [ ${#PCAP_FILES[@]} -eq 0 ]; then
        echo "Error: No .pcap or .pcapng files found in '$INPUT_FOLDER'"
        exit 1
    fi
    
    echo "[1/3] Using ${#PCAP_FILES[@]} capture files from: $INPUT_FOLDER"
    for f in "${PCAP_FILES[@]}"; do
        echo "       - $(basename "$f")"
    done
    
    # Auto-detect port from first file if not specified
    # Prioritize common MAVLink ports (14550, 14540, 14560) if they have traffic
    if [ "$PORT" -eq 0 ]; then
        echo "       Auto-detecting MAVLink port..."
        MAVLINK_PORTS="14550 14540 14560 5760"
        for test_port in $MAVLINK_PORTS; do
            COUNT=$(tshark -r "${PCAP_FILES[0]}" -Y "udp.port == $test_port" 2>/dev/null | wc -l)
            if [ "$COUNT" -gt 100 ]; then
                PORT=$test_port
                echo "       Detected known MAVLink port: $PORT ($COUNT packets in first file)"
                break
            fi
        done
        # Fallback to most common port if no known port found
        if [ "$PORT" -eq 0 ]; then
            PORT=$(tshark -r "${PCAP_FILES[0]}" -Y "udp" -T fields -e udp.dstport 2>/dev/null | \
                   sort | uniq -c | sort -rn | head -1 | awk '{print $2}')
            echo "       Detected port (fallback): $PORT"
        fi
    fi
    
    # Count total packets across all files
    TOTAL_PACKETS=0
    for pcap_file in "${PCAP_FILES[@]}"; do
        COUNT=$(tshark -r "$pcap_file" -Y "udp.port == $PORT" 2>/dev/null | wc -l)
        TOTAL_PACKETS=$((TOTAL_PACKETS + COUNT))
    done
    echo "       Total packets across all files: $TOTAL_PACKETS"
    PACKET_COUNT=$TOTAL_PACKETS

elif [ -n "$INPUT_PCAP" ]; then
    if [ ! -f "$INPUT_PCAP" ]; then
        echo "Error: Input file '$INPUT_PCAP' not found"
        exit 1
    fi
    
    PCAP_FILES+=("$INPUT_PCAP")
    echo "[1/3] Using existing capture: $INPUT_PCAP"
    CAPTURE_FILE="$INPUT_PCAP"
    
    # Auto-detect port if not specified
    # Prioritize common MAVLink ports (14550, 14540, 14560) if they have traffic
    if [ "$PORT" -eq 0 ]; then
        echo "       Auto-detecting MAVLink port..."
        MAVLINK_PORTS="14550 14540 14560 5760"
        for test_port in $MAVLINK_PORTS; do
            COUNT=$(tshark -r "$CAPTURE_FILE" -Y "udp.port == $test_port" 2>/dev/null | wc -l)
            if [ "$COUNT" -gt 100 ]; then
                PORT=$test_port
                echo "       Detected known MAVLink port: $PORT ($COUNT packets)"
                break
            fi
        done
        # Fallback to most common port if no known port found
        if [ "$PORT" -eq 0 ]; then
            PORT=$(tshark -r "$CAPTURE_FILE" -Y "udp" -T fields -e udp.dstport 2>/dev/null | \
                   sort | uniq -c | sort -rn | head -1 | awk '{print $2}')
            echo "       Detected port (fallback): $PORT"
        fi
    fi
    
    PACKET_COUNT=$(tshark -r "$CAPTURE_FILE" -Y "udp.port == $PORT" 2>/dev/null | wc -l)
    echo "       Found $PACKET_COUNT packets on port $PORT"
else
    echo "[1/3] Capturing UDP traffic on port $PORT for ${DURATION}s..."
    echo "       (Make sure MAVLink traffic is flowing!)"
    sudo tcpdump -i any -w "$CAPTURE_FILE" \
        "udp port $PORT" \
        -G "$DURATION" -W 1 2>/dev/null

    if [ ! -f "$CAPTURE_FILE" ]; then
        echo "Error: No packets captured. Is MAVLink traffic flowing on port $PORT?"
        exit 1
    fi

    PACKET_COUNT=$(tcpdump -r "$CAPTURE_FILE" 2>/dev/null | wc -l)
    echo "       Captured $PACKET_COUNT packets"
fi

if [ "$PACKET_COUNT" -lt 100 ]; then
    echo "Warning: Only $PACKET_COUNT packets captured. Dictionary may be poor quality."
    echo "         Recommend capturing at least 1000 packets for best results."
fi

# Helper function to extract payloads from a pcap file as individual sample files
# Usage: extract_packets_to_files <pcap_file> <filter> <output_dir> <start_count> [max_samples]
# Returns: number of packets extracted (via global EXTRACTED_COUNT)
# Note: Limits to max_samples to avoid ARG_MAX overflow with zstd --train
EXTRACTED_COUNT=0
MAX_SAMPLES_PER_DIR=${MAX_SAMPLES_PER_DIR:-20000}  # Limit to avoid ARG_MAX issues

extract_packets_to_files() {
    local pcap_file="$1"
    local filter="$2"
    local output_dir="$3"
    local start_count="${4:-0}"
    local max_samples="${5:-$MAX_SAMPLES_PER_DIR}"
    local count=$start_count
    
    while IFS= read -r hex_data; do
        [ -z "$hex_data" ] && continue
        count=$((count + 1))
        echo "$hex_data" | xxd -r -p > "$output_dir/pkt_$(printf '%06d' $count).bin"
        # Stop if we've reached max samples
        [ "$count" -ge "$max_samples" ] && break
    done < <(tshark -r "$pcap_file" -Y "$filter" -T fields -e udp.payload 2>/dev/null | grep -v '^$')
    
    EXTRACTED_COUNT=$count
}

# Step 2: Extract UDP payload data as individual packet files
echo "[2/3] Extracting UDP payloads as individual files (bidirectional)..."

SAMPLE_COUNT=0

# If we have multiple pcap files (folder mode), process each
if [ ${#PCAP_FILES[@]} -gt 0 ]; then
    for pcap_file in "${PCAP_FILES[@]}"; do
        echo "       Processing: $(basename "$pcap_file")"
        extract_packets_to_files "$pcap_file" "udp.port == $PORT" "$SAMPLES_DIR" "$SAMPLE_COUNT"
        SAMPLE_COUNT=$EXTRACTED_COUNT
    done
else
    # Single file mode (legacy path for live capture)
    extract_packets_to_files "$CAPTURE_FILE" "udp.port == $PORT" "$SAMPLES_DIR" 0
    SAMPLE_COUNT=$EXTRACTED_COUNT
fi

PAYLOAD_SIZE=$(find "$SAMPLES_DIR" -name "*.bin" -exec cat {} + 2>/dev/null | wc -c)
echo "       Extracted $SAMPLE_COUNT packets, ${PAYLOAD_SIZE} bytes total"

if [ "$SAMPLE_COUNT" -lt 100 ]; then
    echo "Warning: Only ${SAMPLE_COUNT} packets captured. Recommend 1000+ for good dictionary."
fi

# Step 3: Train Zstd dictionary using individual packet files
echo "[3/3] Training Zstd dictionary (max size: ${DICT_SIZE} bytes)..."

if [ "$SAMPLE_COUNT" -gt 100 ]; then
    # Train on individual packet files (use find to handle large number of files)
    find "$SAMPLES_DIR" -name "*.bin" -print0 | xargs -0 zstd --train --maxdict=${DICT_SIZE} -o "$OUTPUT_DICT" 2>&1 | grep -E "(Trying|Save|k=)" || true
else
    echo "Warning: Only $SAMPLE_COUNT packets, need 100+ for good training"
    find "$SAMPLES_DIR" -name "*.bin" -print0 | xargs -0 zstd --train --maxdict=${DICT_SIZE} -o "$OUTPUT_DICT" 2>&1 || true
fi

if [ ! -f "$OUTPUT_DICT" ]; then
    echo "Error: Dictionary training failed"
    exit 1
fi

DICT_ACTUAL_SIZE=$(stat -f%z "$OUTPUT_DICT" 2>/dev/null || stat -c%s "$OUTPUT_DICT" 2>/dev/null)
echo ""
echo "=== Results ==="
echo "Dictionary created: $OUTPUT_DICT ($DICT_ACTUAL_SIZE bytes)"
echo ""

# Test per-packet compression
echo "Per-packet compression test (up to 500 packets):"
TOTAL_ORIG=0 TOTAL_COMP=0 COUNT=0
LARGE_ORIG=0 LARGE_COMP=0 LARGE_COUNT=0

for pkt in "$SAMPLES_DIR"/pkt_*.bin; do
    [ -f "$pkt" ] || continue
    COUNT=$((COUNT + 1))
    [ $COUNT -gt 500 ] && break
    
    SZ=$(stat -f%z "$pkt" 2>/dev/null || stat -c%s "$pkt" 2>/dev/null)
    COMP_SZ=$(zstd -D "$OUTPUT_DICT" -3 "$pkt" -c 2>/dev/null | wc -c)
    
    TOTAL_ORIG=$((TOTAL_ORIG + SZ))
    TOTAL_COMP=$((TOTAL_COMP + COMP_SZ))
    
    if [ "$SZ" -ge 50 ]; then
        LARGE_ORIG=$((LARGE_ORIG + SZ))
        LARGE_COMP=$((LARGE_COMP + COMP_SZ))
        LARGE_COUNT=$((LARGE_COUNT + 1))
    fi
done

if [ "$TOTAL_ORIG" -gt 0 ]; then
    awk "BEGIN {
        ratio = $TOTAL_ORIG / $TOTAL_COMP
        savings = (1 - $TOTAL_COMP/$TOTAL_ORIG) * 100
        printf \"  All packets:    %d -> %d bytes (%.2fx, %.1f%% %s)\\n\", 
            $TOTAL_ORIG, $TOTAL_COMP, ratio, (savings<0?-savings:savings), (savings<0?\"expansion\":\"reduction\")
    }"
fi
if [ "$LARGE_ORIG" -gt 0 ]; then
    awk "BEGIN {
        ratio = $LARGE_ORIG / $LARGE_COMP
        savings = (1 - $LARGE_COMP/$LARGE_ORIG) * 100
        printf \"  Packets >=50B:  %d -> %d bytes (%.2fx, %.1f%% %s) [%d packets]\\n\", 
            $LARGE_ORIG, $LARGE_COMP, ratio, (savings<0?-savings:savings), (savings<0?\"expansion\":\"reduction\"), $LARGE_COUNT
    }"
fi

echo ""
echo "Usage with mavlink-router:"
echo "  install $OUTPUT_DICT to the path referenced by an endpoint's ZstdDictionary option,"
echo "  e.g. /usr/share/mavlink-router/mavlink_zstd.dict, and set on that [UdpEndpoint]:"
echo "      ZstdCompression = true"
echo "      ZstdDictionary  = /usr/share/mavlink-router/mavlink_zstd.dict"
echo "  Use the same dictionary on both ends of the link."

echo ""
echo "To capture and train:"
echo "  $0 14550 120 mavlink.dict                    # Capture for 120s"
echo "  $0 --pcap capture.pcap 0 mavlink.dict        # From single pcap (auto-detect port)"
echo "  $0 --folder /path/to/pcaps 0 mavlink.dict    # From folder of pcaps"
