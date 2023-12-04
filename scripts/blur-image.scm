(define (blur-image input-file output-file radius)
  (let* ((image (car (gimp-file-load RUN-NONINTERACTIVE input-file input-file)))
       (drawable (car (gimp-image-get-active-layer image))))
    (plug-in-gauss RUN-NONINTERACTIVE image drawable radius radius 0)
    (gimp-file-save RUN-NONINTERACTIVE image drawable output-file output-file)
    (gimp-image-delete image)
  )
)

(script-fu-register
  "blur-image"                    ;function name
  "Blur"                          ;menu label
  "Apply a gaussian blur effect"  ;desc
  "Matthieu Bouron"               ;author
  "Copyright Nope Forge"          ;copyright
  "2023"                          ;date created
  ""                              ;image type that the script works on
  SF-STRING "Input"  "Text Box"   ;input variable
  SF-STRING "Output" "Text Box"   ;output variable
  SF-VALUE  "Radius" "1"          ;radius variable
)
