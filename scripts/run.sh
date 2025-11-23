#!/bin/bash

ITER=${1:-10}

#### Testing History Store ####
# pushd ../build
# cmake .  -DENABLE_VERSION_STORE=OFF
# make -j || exit
# popd

# pushd ../build/examples/c/ex_history_store
# ./ex_history_store
# echo ""
# ../../../wt -h ./WT_HOME/ dump -p file:WiredTigerHS.wt
# echo ""
# popd
# 

#### Testing History Store with VID ####
# pushd ../build
# cmake .  -DENABLE_VERSION_STORE=OFF
# make -j || exit
# popd

# rm input.txt
# rm output.txt
# rm sorted.txt

# pushd ../build/examples/c/ex_insert_with_vid
# echo ""
# echo "Insert with VID called"
# echo ""
# ./ex_insert_with_vid $ITER
# echo ""
# echo "Dump Version Store"
# echo ""
# ../../../wt -h ./WT_HOME/ dump -p file:WiredTigerHS.wt > input.txt
# mv input.txt ../../../../scripts/
# popd

# ./check.sh $ITER

#### Testing Version Store with VID ####
pushd ../build
cmake .  -DENABLE_VERSION_STORE=ON
make -j || exit
popd

rm input.txt
rm output.txt
rm sorted.txt

pushd ../build/examples/c/ex_insert_with_vid
echo ""
echo "Insert with VID called"
echo ""
./ex_insert_with_vid $ITER
echo ""
echo "Dump Version Store"
echo ""
../../../wt -h ./WT_HOME/ dump -p file:WiredTigerVS.wt > input.txt
mv input.txt ../../../../scripts/
popd

./check.sh $ITER