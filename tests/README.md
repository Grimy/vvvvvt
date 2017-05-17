Tests in this directory are simple text files. To run a test, `cat` it into
vvvvvt, `cat` it into XTerm, and visually compare the rendered outputs (other
terminals fail most of the tests, so they can’t be used as a reference). 

This approach cannot cover user interaction, such as selecting text and
scrolling. But it covers UTF-8 handling, parsing and execution of escape
sequences, and rendering, which is about 80% of what a terminal does.
