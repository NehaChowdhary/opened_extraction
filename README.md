## Install
docker build . -t opened/extract:0.01

## Run
### Phase I
1. docker run -it  --mount type=bind,src=/home/sayandes/extraction/examples/katran,dst=/root/examples/katran --mount type=bind,src=/home/sayandes/opened_extraction/op,dst=/root/op opened/extract:0.01
2. python3 extraction_runner.py -f <function_name> -s <source_folder> -o <txl_output>, an example is given in run1.sh

### Phase II
1. Open the func.out file and remove the duplicate function definitions

### Phase III
2. python3 function-extractor.py -o/--opdir, -f/--codequeryOutputFile, -e/--extractedFileName, -c/--cscopeFile, -t/--txlDir, -s/--srcdir, an example is given in run2.sh
