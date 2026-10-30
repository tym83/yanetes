// protohints lists what protobuf alone does not tell about the Kubernetes
// API types, one "message.field" a line, for yapi's protobuf decoder:
//
//   - pointer-strings.txt: *string fields, where "" is a value and not an
//     absence (protobuf writes both the same way);
//   - inline-fields.txt: embedded structs JSON inlines (Volume's
//     VolumeSource), which protobuf keeps as a field of their own.
//
//	go run ./hack/protohints $(go list -m -f '{{.Dir}}' k8s.io/api) <proto-dir>
package main

import (
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
)

func protoName(tag *ast.BasicLit) string {
	if tag == nil {
		return ""
	}
	st := reflect.StructTag(strings.Trim(tag.Value, "`"))
	for _, p := range strings.Split(st.Get("protobuf"), ",") {
		if strings.HasPrefix(p, "name=") {
			return strings.TrimPrefix(p, "name=")
		}
	}
	return ""
}

func main() {
	root, out := os.Args[1], os.Args[2]
	var pointers, inlines []string
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
				name := protoName(field.Tag)
				if name == "" {
					continue
				}
				// An embedded struct without a JSON name of its own is inlined,
				// whether the tag says ",inline" or, lately, just "".
				jsonName, _, _ := strings.Cut(reflect.StructTag(strings.Trim(field.Tag.Value, "`")).Get("json"), ",")
				if len(field.Names) == 0 && jsonName == "" {
					inlines = append(inlines, pkg+"."+ts.Name.Name+"."+name)
					continue
				}
				if star, ok := field.Type.(*ast.StarExpr); ok {
					if id, ok := star.X.(*ast.Ident); ok && id.Name == "string" {
						pointers = append(pointers, pkg+"."+ts.Name.Name+"."+name)
					}
				}
			}
			return true
		})
		return nil
	})
	write := func(file string, lines []string) {
		sort.Strings(lines)
		if err := os.WriteFile(filepath.Join(out, file), []byte(strings.Join(lines, "\n")+"\n"), 0o644); err != nil {
			panic(err)
		}
	}
	write("pointer-strings.txt", pointers)
	write("inline-fields.txt", inlines)
}
