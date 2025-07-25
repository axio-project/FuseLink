#!/bin/bash
#SBATCH --job-name=fuselink_serving_30b
#SBATCH --output=fuselink_serving-%j.out
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:30:00
#SBATCH --gpus-per-task=2
#SBATCH --partition=normal
#SBATCH --account=fuselink


module purge
module load slurm
module load cuda12.2
module load nvhpc-hpcx-cuda12

# source activate fuselink


# read data path from $1
DATA_PATH=/project/fuselink/rzh/FuseLinkTest/serving/tracegen/trace_1.txt

BATCH_SZ=8
MODEL_PATH=dummy
#MODEL_NAME=opt_125m
MODEL_NAME=opt_13b
#MODEL_NAME=opt_125m
# MODEL_NAME=llama2_7b
# MODEL_NAME=llama2_7b
PRECISION=fp16
INPUT_LEN=4096
BLOCK_SZ=32
NUM_DECODE_STEP=1
TP_SZ=1
export CUDA_VISIBLE_DEVICES=0
# calcualte the number of processes
NUM_PROCESS=$((2*TP_SZ))
# run the test
# --prefix $HOME/anaconda3/envs/distserve \

cd /project/fuselink/rzh/FuseLinkTest/serving/build

srun "$(hostname),slot=$TP_SZ" > hostnames.txt

mpirun -np $NUM_PROCESS \
    --hostfile hostnames.txt \
    -x NCCL_DEBUG=WARN \
    -x NCCL_NET_GDR_LEVEL=SYS \
    -x NCCL_P2P_NET_CHUNKSIZE=524288 \
    ./FuseLinkOnlineServeTest \
    --tracepath $DATA_PATH \
    --batch_size $BATCH_SZ \
    --model_path $MODEL_PATH \
    --model_name $MODEL_NAME \
    --precision $PRECISION \
    --input_len $INPUT_LEN \
    --block_size $BLOCK_SZ \
    --num_decoding_step $NUM_DECODE_STEP \
    --tp_size $TP_SZ