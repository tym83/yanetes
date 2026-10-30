// pointerstrings lists the *string fields of the Kubernetes API types as
// protobuf field names, one "message.field" a line. Protobuf cannot tell
// a *string set to "" from a string left empty; yapi keeps "" for these.
//
//	go run ./hack/pointerstrings $(go list -m -f '{{.Dir}}' k8s.io/api) > <proto-dir>/pointer-strings.txt
package main

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
)

func main() {
	root := os.Args[1]
	var out []string
	_ = filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() || filepath.Base(path) != "types.go" {
			return nil
		}
		rel, _ := filepath.Rel(root, filepath.Dir(path))
		parts := strings.Split(rel, string(filepath.Separator))
		if len(parts) != 2 {
			return nil
		}
		pkg := "k8s.io.api." + parts[0] + "." + parts[1]
		f, err := parser.ParseFile(token.NewFileSet(), path, nil, 0)
		if err != nil {
			return nil
		}
		ast.Inspect(f, func(n ast.Node) bool {
			ts, ok := n.(*ast.TypeSpec)
			if !ok {
				return true
			}
			st, ok := ts.Type.(*ast.StructType)
			if !ok {
				return true
			}
			for _, field := range st.Fields.List {
				star, ok := field.Type.(*ast.StarExpr)
				if !ok || field.Tag == nil || len(field.Names) == 0 {
					continue
				}
				if id, ok := star.X.(*ast.Ident); !ok || id.Name != "string" {
					continue
				}
				tag := reflect.StructTag(strings.Trim(field.Tag.Value, "`"))
				name := strings.Split(tag.Get("protobuf"), ",")
				for _, p := range name {
					if strings.HasPrefix(p, "name=") {
						out = append(out, pkg+"."+ts.Name.Name+"."+strings.TrimPrefix(p, "name="))
					}
				}
			}
			return true
		})
		return nil
	})
	sort.Strings(out)
	fmt.Println(strings.Join(out, "\n"))
}
